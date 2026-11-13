#!/usr/bin/env python3
"""Runs KisakCOD-vulkan-shaders (src/gfx_vk/tools/vk_shader_tool.cpp) and checks it.

The Legacy shader translator turns D3D9 shader bytecode into SPIR-V through
dxbc-spirv. These checks need the built tool and spirv-val (SPIRV-Tools); no
GPU, game data or network. The shaders are the shader spike's 81 op-by-op test
cases (scripts/tools/shader_spike/cases.py), assembled here, plus a few written
for linking. The checks:

  usage      bad arguments exit with status 2; bytes that are not D3D9
             bytecode fail with status 1 and a reason
  cases      every case translates and gives SPIR-V that spirv-val accepts for
             Vulkan 1.3 with scalar block layout, except the five that use
             predication, which are refused with a reason (dxbc-spirv at the
             pinned commit mistranslates them; the game never uses them)
  layout     every uniform block sits at its Legacy binding with the offsets
             and strides of the structs in vk_legacy_shader.h, constant arrays
             are cut to the registers the reflection reports, and textures and
             samplers sit at their bindings
  reflection constants, samplers, inputs and outputs of chosen cases
  sm1        hand-assembled vs_1_1 and ps_1_1 translate and link; ps 1.x
             samplers pick their texture type at run time
  linking    a pixel shader linked to a vertex shader that writes its inputs at
             other locations reads them from there, and one the vertex shader
             does not write is dropped
  state      the legacy state the adapter never changes (fog, clip planes,
             point sprites, unbound and gather samplers) is constant, and
             with KISAK_VK_LEGACY_STATE=dynamic is read from its blocks, whose
             layout is checked too
  depth      --depth-samplers (depth compare fixed at build time, as the
             adapter builds pixel shaders by default): every case with samplers
             still passes spirv-val and reads no sampler state block, with
             no depth-reference sampling when no sampler is a depth one, and
             only depth-reference sampling of 2D and cube images when all are
  linear     --linear-output (the Modern path's linear-light scene): every
             pixel shader case that writes colour 0 still passes spirv-val and
             gets a clamp, a pow and a select per colour channel it writes
  lit        --linear-inputs with everything marked (the Modern path's lit
             surfaces): every pixel shader case still passes spirv-val, and
             those that read a colour get more pows than without

Usage:
  test_vulkan_legacy_shaders.py --binary build/bin/KisakCOD-vulkan-shaders
      [--spirv-val spirv-val | --no-spirv-val]

--no-spirv-val skips only the spirv-val runs (the Windows job has no
SPIRV-Tools); every other check still reads the SPIR-V itself.
"""

import argparse
import os
import re
import shutil
import struct
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
sys.path.insert(0, os.path.join(ROOT, 'scripts', 'tools', 'shader_spike'))
import cases as shader_cases  # noqa: E402
import d3d9_asm  # noqa: E402

PREDICATED = {'ps3_setp_predicated', 'ps3_predicate_not', 'ps3_if_predicate', 'ps3_breakp',
              'ps2x_dsx_predicate'}

# vk_legacy_shader.h: Uniform order, bindings and block layouts (scalar layout).
UNIFORMS = ['float_constants', 'int_constants', 'bool_constants', 'alpha_test', 'fog', 'clip_planes',
            'point_args', 'sampler_state', 'texture_stage']
# uniform index -> ('array', stride, element member offsets or None) or ('struct', member offsets)
LAYOUTS = {
    0: ('array', 16, None),
    1: ('array', 16, None),
    2: ('array', 4, None),
    3: ('struct', [0, 4, 8]),
    4: ('struct', [0, 4, 8, 12, 24, 28, 32]),
    5: ('struct', [0, 4, 20, 36, 52, 68, 84]),
    6: ('struct', [0, 4, 8, 12]),
    7: ('array', 28, [0, 4, 8, 12, 16, 20, 24]),
    8: ('array', 24, [0, 8, 16, 20]),
}


def uniform_binding(stage, uniform):
    return (0 if stage == 'vs' else 16) + uniform


def image_binding(stage, sampler):
    return (32 if stage == 'vs' else 0) + sampler


def sampler_binding(stage, sampler):
    return (48 if stage == 'vs' else 16) + sampler


# ---------------------------------------------------------------------------
# A small SPIR-V reader: decorations, types and variables.
# ---------------------------------------------------------------------------
OP_NAME, OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR = 5, 21, 22, 23
OP_TYPE_ARRAY, OP_TYPE_STRUCT, OP_TYPE_POINTER, OP_CONSTANT = 28, 30, 32, 43
OP_VARIABLE, OP_DECORATE, OP_MEMBER_DECORATE = 59, 71, 72
OP_EXT_INST, OP_SELECT = 12, 169
DEC_ARRAY_STRIDE, DEC_LOCATION, DEC_BINDING, DEC_SET, DEC_OFFSET = 6, 30, 33, 34, 35
SC_INPUT, SC_UNIFORM_CONSTANT, SC_UNIFORM, SC_OUTPUT = 1, 0, 2, 3
# OpKill, OpTerminateInvocation, OpDemoteToHelperInvocation
OPS_DISCARD = (252, 4416, 5380)
# OpImageSample{Implicit,Explicit}Lod, OpImageSampleProj{Implicit,Explicit}Lod, OpImageGather
OPS_SAMPLE_PLAIN = (87, 88, 91, 92, 96)
# OpImageSampleDref{Implicit,Explicit}Lod, OpImageSampleProjDref{Implicit,Explicit}Lod, OpImageDrefGather
OPS_SAMPLE_DREF = (89, 90, 93, 94, 97)


class Spirv:
    def __init__(self, data):
        words = struct.unpack('<%dI' % (len(data) // 4), data)
        if words[0] != 0x07230203:
            raise ValueError('not SPIR-V')
        self.decorations = {}   # id -> {decoration: value}
        self.member_offsets = {}  # struct id -> {member: offset}
        self.types = {}         # id -> (opcode, operands)
        self.constants = {}     # id -> value
        self.variables = []     # (id, pointer type id, storage class)
        self.opcodes = {}       # opcode -> count
        i = 5
        while i < len(words):
            count, opcode = words[i] >> 16, words[i] & 0xffff
            ops = words[i + 1:i + count]
            self.opcodes[opcode] = self.opcodes.get(opcode, 0) + 1
            if opcode == OP_DECORATE and len(ops) >= 3:
                self.decorations.setdefault(ops[0], {})[ops[1]] = ops[2]
            elif opcode == OP_DECORATE and len(ops) == 2:
                self.decorations.setdefault(ops[0], {})[ops[1]] = True
            elif opcode == OP_MEMBER_DECORATE and ops[2] == DEC_OFFSET:
                self.member_offsets.setdefault(ops[0], {})[ops[1]] = ops[3]
            elif opcode in (OP_TYPE_INT, OP_TYPE_FLOAT, OP_TYPE_VECTOR, OP_TYPE_ARRAY, OP_TYPE_STRUCT,
                            OP_TYPE_POINTER):
                self.types[ops[0]] = (opcode, ops[1:])
            elif opcode == OP_CONSTANT:
                self.constants[ops[1]] = ops[2]
            elif opcode == OP_VARIABLE:
                self.variables.append((ops[1], ops[0], ops[2]))
            i += count

    def pointee(self, pointer_type):
        opcode, ops = self.types[pointer_type]
        assert opcode == OP_TYPE_POINTER
        return ops[1]

    def bindings(self):
        """(set, binding) -> (variable id, pointee type id, storage class)"""
        out = {}
        for var, ptype, storage in self.variables:
            dec = self.decorations.get(var, {})
            if DEC_BINDING in dec:
                out[(dec.get(DEC_SET, 0), dec[DEC_BINDING])] = (var, self.pointee(ptype), storage)
        return out

    def locations(self, storage_class):
        out = []
        for var, _, storage in self.variables:
            dec = self.decorations.get(var, {})
            if storage == storage_class and DEC_LOCATION in dec:
                out.append(dec[DEC_LOCATION])
        return sorted(out)


def check_block(spv, struct_id, layout):
    """Returns '' when the block matches the layout, else what differs."""
    kind = layout[0]
    offsets = spv.member_offsets.get(struct_id, {})
    if kind == 'struct':
        got = [offsets.get(m) for m in range(len(layout[1]))]
        return '' if got == layout[1] else 'member offsets %s, expected %s' % (got, layout[1])
    # A block holding one array.
    members = spv.types[struct_id][1]
    if len(members) != 1 or offsets.get(0) != 0:
        return 'expected one array member at offset 0'
    opcode, ops = spv.types[members[0]]
    if opcode != OP_TYPE_ARRAY:
        return 'member is not an array'
    stride = spv.decorations.get(members[0], {}).get(DEC_ARRAY_STRIDE)
    if stride != layout[1]:
        return 'array stride %s, expected %s' % (stride, layout[1])
    if layout[2] is not None:
        element = ops[0]
        got = [spv.member_offsets.get(element, {}).get(m) for m in range(len(layout[2]))]
        if got != layout[2]:
            return 'element member offsets %s, expected %s' % (got, layout[2])
    return ''


def array_length(spv, struct_id):
    members = spv.types[struct_id][1]
    return spv.constants.get(spv.types[members[0]][1][1])


# ---------------------------------------------------------------------------

class Checks:
    def __init__(self):
        self.failures = []

    def expect(self, name, condition, detail=''):
        print(f"{'ok  ' if condition else 'FAIL'} {name}{': ' + detail if detail and not condition else ''}")
        if not condition:
            self.failures.append(name)


def run(binary, args, timeout=120, env=None):
    result = subprocess.run([binary] + args, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True,
                            timeout=timeout, env=env)
    return result.returncode, result.stdout, result.stderr


def parse(stdout):
    """key=value lines; repeated keys (input, output) become lists."""
    out = {}
    for line in stdout.splitlines():
        if line.startswith('legacy_shader') and '=' in line:
            key, _, value = line.partition('=')
            key = key[len('legacy_shader.'):] if key != 'legacy_shader' else 'result'
            if key in ('input', 'output'):
                out.setdefault(key, []).append(value)
            else:
                out[key] = value
    return out


def write(path, source):
    with open(path, 'wb') as f:
        f.write(d3d9_asm.assemble(source))


# Shader model 1.1, hand-assembled (the spike's assembler starts at 2.0):
#   vs_1_1: dcl_position v0; mov oPos, v0
#   ps_1_1: tex t0; mov r0, t0
VS11 = struct.pack('<7I', 0xFFFE0101, 0x0000001F, 0x80000000, 0x900F0000, 0x00000001, 0xC00F0000, 0x90E40000) + \
    struct.pack('<I', 0x0000FFFF)
PS11 = struct.pack('<7I', 0xFFFF0101, 0x00000042, 0xB00F0000, 0x00000001, 0x800F0000, 0xB0E40000, 0x0000FFFF)

LINK_VS = """vs_3_0
dcl_position v0
dcl_texcoord0 v1
dcl_position o0
dcl_tangent o1
dcl_binormal o2
dcl_texcoord0 o3
m4x4 o0, v0, c0
mov o1, v1
mul o2, v1, c4
add o3, v1, c5
"""
# Reads binormal (vs location 13), and texcoord5, which the vertex shader does not write.
LINK_PS = """ps_3_0
dcl_binormal v0
dcl_texcoord5 v1
add r0, v0, v1
mov oC0, r0
"""


def main():
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('--binary', required=True)
    parser.add_argument('--spirv-val', default='spirv-val')
    parser.add_argument('--no-spirv-val', action='store_true')
    args = parser.parse_args()
    binary = os.path.abspath(args.binary)
    spirv_val = None if args.no_spirv_val else (shutil.which(args.spirv_val) or args.spirv_val)

    def validate(path):
        """'' when spirv-val accepts the file (or is skipped), else its output."""
        if spirv_val is None:
            return ''
        val = subprocess.run([spirv_val, '--target-env', 'vulkan1.3', '--scalar-block-layout', path],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        return '' if val.returncode == 0 else (val.stdout.strip() or 'spirv-val failed')[:400]
    checks = Checks()
    # The cases are translated with the legacy state fixed, then dynamic.
    os.environ.pop('KISAK_VK_LEGACY_STATE', None)

    with tempfile.TemporaryDirectory() as work:
        # usage
        for bad in (['translate'], ['frobnicate', 'x'], ['translate', 'a', '--bogus'], ['batch']):
            code, _, _ = run(binary, bad)
            checks.expect(f"usage {' '.join(bad)}", code == 2, f'status {code}')
        junk = os.path.join(work, 'junk.bin')
        with open(junk, 'wb') as f:
            f.write(b'\x01\x02\x03\x04' * 8)
        code, stdout, _ = run(binary, ['translate', junk])
        checks.expect('junk is refused', code == 1 and 'not D3D9 shader bytecode' in stdout, stdout.strip())

        # cases
        case_dir = os.path.join(work, 'cases')
        os.makedirs(case_dir)
        for name, source in shader_cases.all_cases().items():
            write(os.path.join(case_dir, name + '.bin'), source)
        code, stdout, _ = run(binary, ['batch', case_dir, '--threads', '4'], timeout=600)
        print(stdout, end='')
        batch = parse(stdout)
        total = len(shader_cases.all_cases())
        checks.expect('batch counts', batch.get('batch') == 'files %d ok %d failed %d'
                      % (total, total - len(PREDICATED), len(PREDICATED)), batch.get('batch', 'missing'))
        checks.expect('batch failures are predication only', batch.get('batch_failure', '').startswith(
            '%d predication' % len(PREDICATED)), batch.get('batch_failure', 'missing'))

        results = {}
        for name in sorted(shader_cases.all_cases()):
            spv = os.path.join(work, name + '.spv')
            code, stdout, _ = run(binary, ['translate', os.path.join(case_dir, name + '.bin'), '--out', spv])
            found = parse(stdout)
            if name in PREDICATED:
                checks.expect(f'{name} refused', code == 1 and 'predication' in found.get('result', ''),
                              stdout.strip())
                continue
            if code != 0 or found.get('result') != 'passed':
                checks.expect(f'{name} translates', False, stdout.strip())
                continue
            problem = validate(spv)
            if problem:
                checks.expect(f'{name} spirv-val', False, problem)
                continue
            with open(spv, 'rb') as f:
                results[name] = (found, Spirv(f.read()))
        checks.expect('every unpredicated case translated' + (' to valid SPIR-V' if spirv_val else ''),
                      len(results) == total - len(PREDICATED), f'{len(results)} of {total - len(PREDICATED)}')

        # The same with KISAK_VK_LEGACY_STATE=dynamic: fog, clip planes, point
        # sprites and the sampler state the adapter never changes stay uniforms.
        dynamic_env = dict(os.environ, KISAK_VK_LEGACY_STATE='dynamic')
        dynamic = {}
        for name in sorted(results):
            spv = os.path.join(work, name + '-dynamic.spv')
            code, stdout, _ = run(binary, ['translate', os.path.join(case_dir, name + '.bin'), '--out', spv],
                                  env=dynamic_env)
            found = parse(stdout)
            if code != 0 or found.get('result') != 'passed' or found.get('legacy_state') != 'dynamic':
                checks.expect(f'{name} translates with the legacy state dynamic', False, stdout.strip())
                continue
            problem = validate(spv)
            if problem:
                checks.expect(f'{name} spirv-val with the legacy state dynamic', False, problem)
                continue
            with open(spv, 'rb') as f:
                dynamic[name] = (found, Spirv(f.read()))
        checks.expect('every unpredicated case translated with the legacy state dynamic',
                      len(dynamic) == len(results), f'{len(dynamic)} of {len(results)}')

        # layout: every uniform block, texture and sampler at its binding
        layout_problems = []
        layout_cases = [(name, entry) for name, entry in sorted(results.items())]
        layout_cases += [(name + ' (dynamic state)', entry) for name, entry in sorted(dynamic.items())]
        for name, (found, spv) in layout_cases:
            stage = found['stage'].split()[0]
            declared = set(found['uniforms'].split(',')) - {'none'}
            bindings = spv.bindings()
            for uniform, uname in enumerate(UNIFORMS):
                entry = bindings.get((0, uniform_binding(stage, uniform)))
                if (entry is not None) != (uname in declared):
                    layout_problems.append(f'{name}: {uname} declared {uname in declared}, bound {entry is not None}')
                    continue
                if entry is None:
                    continue
                var, struct_id, storage = entry
                problem = check_block(spv, struct_id, LAYOUTS[uniform]) if storage == SC_UNIFORM else 'not Uniform'
                if problem:
                    layout_problems.append(f'{name}: {uname}: {problem}')
                if uniform == 0:
                    want = int(re.match(r'float (\d+)', found['constants']).group(1))
                    if array_length(spv, struct_id) != want:
                        layout_problems.append(f'{name}: float constants length {array_length(spv, struct_id)}, '
                                               f'reflection {want}')
            samplers = [] if found['samplers'] == 'none' else [int(s.split(':')[0]) for s in found['samplers'].split(',')]
            for s in samplers:
                if (1, image_binding(stage, s)) not in bindings or (1, sampler_binding(stage, s)) not in bindings:
                    layout_problems.append(f'{name}: sampler {s} not at its bindings')
            stray = [k for k in bindings if k[0] not in (0, 1)]
            if stray:
                layout_problems.append(f'{name}: descriptors in other sets {stray}')
        checks.expect('uniform blocks, textures and samplers match the Legacy layout', not layout_problems,
                      '\n  '.join(layout_problems[:20]))

        # depth: depth compare fixed per sampler at build time
        code, _, _ = run(binary, ['translate', os.path.join(case_dir, 'ps3_texld_cube.bin'), '--depth-samplers', 'x'])
        checks.expect('usage --depth-samplers x', code == 2, f'status {code}')
        depth_problems, depth_cases = [], 0
        for name, (found, _) in sorted(results.items()):
            if found['samplers'] == 'none':
                continue
            depth_cases += 1
            types = {t for t in (s.split(':')[1] for s in found['samplers'].split(','))}
            for mask in ('0', '0xffff'):
                spv = os.path.join(work, f'{name}-depth-{mask}.spv')
                code, stdout, _ = run(binary, ['translate', os.path.join(case_dir, name + '.bin'),
                                               '--depth-samplers', mask, '--out', spv])
                if code != 0 or parse(stdout).get('result') != 'passed':
                    depth_problems.append(f'{name} {mask}: {stdout.strip()[:200]}')
                    continue
                problem = validate(spv)
                if problem:
                    depth_problems.append(f'{name} {mask}: spirv-val: {problem}')
                    continue
                with open(spv, 'rb') as f:
                    spirv = Spirv(f.read())
                ops = spirv.opcodes
                # Fixed with the clamp too, shader model 2 and 3 leave nothing
                # to read in the sampler state block (the texture type is for 1.x).
                stage = found['stage'].split()[0]
                block = (0, uniform_binding(stage, UNIFORMS.index('sampler_state'))) in spirv.bindings()
                if block and not found['stage'].endswith(('1.0', '1.1', '1.2', '1.3', '1.4')):
                    depth_problems.append(f'{name} {mask}: still reads the sampler state block')
                dref = sum(ops.get(op, 0) for op in OPS_SAMPLE_DREF)
                plain = sum(ops.get(op, 0) for op in OPS_SAMPLE_PLAIN)
                if mask == '0' and dref:
                    depth_problems.append(f'{name} 0: {dref} depth-reference samples left')
                if mask != '0' and types <= {'2d', 'cube'} and (plain or not dref):
                    depth_problems.append(f'{name} {mask}: {plain} plain and {dref} depth-reference samples')
        checks.expect(f'depth compare fixed at build time ({depth_cases} cases with samplers)',
                      depth_cases > 0 and not depth_problems, '\n  '.join(depth_problems[:20]))

        # alpha test fixed at build time (--alpha-compare, as the adapter builds
        # a pixel shader for one pipeline): with ALWAYS (7) the test's discard
        # goes (a texkill's stays) and the alpha test block is not read; with
        # GREATER (4) the test stays
        alpha_problems = []
        alpha_cases = 0
        for name, (found, base) in sorted(results.items()):
            if not found['stage'].startswith('ps') or 'alpha_test' not in found['uniforms'].split(','):
                continue
            alpha_cases += 1
            base_discards = sum(base.opcodes.get(op, 0) for op in OPS_DISCARD)
            for compare in ('7', '4'):
                spv = os.path.join(work, f'{name}-alpha-{compare}.spv')
                code, stdout, _ = run(binary, ['translate', os.path.join(case_dir, name + '.bin'),
                                               '--alpha-compare', compare, '--out', spv])
                if code != 0 or parse(stdout).get('result') != 'passed':
                    alpha_problems.append(f'{name} {compare}: {stdout.strip()[:200]}')
                    continue
                problem = validate(spv)
                if problem:
                    alpha_problems.append(f'{name} {compare}: spirv-val: {problem}')
                    continue
                with open(spv, 'rb') as f:
                    spirv = Spirv(f.read())
                discards = sum(spirv.opcodes.get(op, 0) for op in OPS_DISCARD)
                block = (0, uniform_binding('ps', UNIFORMS.index('alpha_test'))) in spirv.bindings()
                if compare == '7' and (discards != base_discards - 1 or block):
                    alpha_problems.append(f'{name} always: {discards} of {base_discards} discards, block read {block}')
                if compare == '4' and discards != base_discards:
                    alpha_problems.append(f'{name} greater: {discards} of {base_discards} discards')
        checks.expect(f'alpha test fixed at build time ({alpha_cases} pixel shader cases)',
                      alpha_cases > 0 and not alpha_problems, '\n  '.join(alpha_problems[:20]))

        # linear output (--linear-output, as the adapter builds a pixel shader
        # that draws into the Modern path's linear-light scene): the colour
        # output's red, green and blue each get a clamp and a pow (two more
        # GLSL.std.450 instructions) and a select, and the SPIR-V stays valid
        linear_problems = []
        linear_cases = 0
        for name, (found, base) in sorted(results.items()):
            outputs = found.get('output', [])
            color = next((o for o in outputs if o.startswith('color0 ')), None)
            if not found['stage'].startswith('ps') or color is None:
                continue
            mask = color.rsplit(' ', 1)[-1]
            channels = sum(1 for c in 'xyz' if c in mask)
            if not channels:
                continue
            linear_cases += 1
            spv = os.path.join(work, f'{name}-linear.spv')
            code, stdout, _ = run(binary, ['translate', os.path.join(case_dir, name + '.bin'), '--linear-output',
                                           '--out', spv])
            if code != 0 or parse(stdout).get('result') != 'passed':
                linear_problems.append(f'{name}: {stdout.strip()[:200]}')
                continue
            problem = validate(spv)
            if problem:
                linear_problems.append(f'{name}: spirv-val: {problem}')
                continue
            with open(spv, 'rb') as f:
                spirv = Spirv(f.read())
            ext = spirv.opcodes.get(OP_EXT_INST, 0) - base.opcodes.get(OP_EXT_INST, 0)
            selects = spirv.opcodes.get(OP_SELECT, 0) - base.opcodes.get(OP_SELECT, 0)
            if ext < 2 * channels or selects < channels:
                linear_problems.append(f'{name}: {ext} more extended instructions and {selects} more selects '
                                       f'for {channels} channels')
        checks.expect(f'linear output decodes the colour output ({linear_cases} pixel shader cases)',
                      linear_cases > 0 and not linear_problems, '\n  '.join(linear_problems[:20]))

        # linear inputs (--linear-inputs, as the adapter builds a lit pixel
        # shader for the Modern path's linear-light scene), with every
        # sampler, constant and the colour input marked: the SPIR-V stays
        # valid, and shaders that read colours decode them (more pows)
        lit_problems = []
        lit_cases = 0
        lit_decoded = 0
        for name, (found, base) in sorted(results.items()):
            if not found['stage'].startswith('ps'):
                continue
            lit_cases += 1
            spv = os.path.join(work, f'{name}-lit.spv')
            code, stdout, _ = run(binary, ['translate', os.path.join(case_dir, name + '.bin'), '--linear-inputs',
                                           '0xffff,0xffff,0xffffffffffffffff,1', '--out', spv])
            if code != 0 or parse(stdout).get('result') != 'passed':
                lit_problems.append(f'{name}: {stdout.strip()[:200]}')
                continue
            problem = validate(spv)
            if problem:
                lit_problems.append(f'{name}: spirv-val: {problem}')
                continue
            with open(spv, 'rb') as f:
                spirv = Spirv(f.read())
            if spirv.opcodes.get(OP_EXT_INST, 0) > base.opcodes.get(OP_EXT_INST, 0):
                lit_decoded += 1
        checks.expect(f'linear inputs build valid shaders ({lit_cases} pixel shader cases, {lit_decoded} decode)',
                      lit_cases > 0 and lit_decoded > 0 and not lit_problems, '\n  '.join(lit_problems[:20]))

        # legacy state: fixed by default (no fog, clip plane or point argument
        # block declared, no gather), read at run time with the switch (every
        # vertex shader declares the clip planes, every ps 2.x one the fog,
        # and samples of 2D and cube images have a gather path)
        state_problems = []
        for name, (found, spv) in sorted(results.items()):
            declared = set(found['uniforms'].split(','))
            stray = declared & {'fog', 'clip_planes', 'point_args'}
            if found.get('legacy_state') != 'fixed' or stray or spv.opcodes.get(96, 0):
                state_problems.append(f"{name}: state {found.get('legacy_state')}, blocks {sorted(stray)}, "
                                      f"gathers {spv.opcodes.get(96, 0)}")
        gathers = 0
        for name, (found, spv) in sorted(dynamic.items()):
            declared = set(found['uniforms'].split(','))
            # pixel shaders apply fog below shader model 3 only
            want = 'clip_planes' if found['stage'].startswith('vs') else \
                'fog' if not found['stage'].endswith('3.0') else None
            if want and want not in declared:
                state_problems.append(f'{name} (dynamic state): no {want} block')
            gathers += 1 if spv.opcodes.get(96, 0) else 0
        if not gathers:
            state_problems.append('no case gathers with the legacy state dynamic')
        words_fixed = sum(sum(spv.opcodes.values()) for _, spv in results.values())
        words_dynamic = sum(sum(spv.opcodes.values()) for _, spv in dynamic.values())
        print(f'legacy state: {words_fixed} instructions fixed, {words_dynamic} dynamic')
        checks.expect('legacy state fixed by default and dynamic with the switch',
                      not state_problems and words_fixed < words_dynamic, '\n  '.join(state_problems[:20]))

        # reflection of chosen cases
        def reflected(name, key):
            return results[name][0].get(key) if name in results else None
        checks.expect('ps3_add reads c4 only (5 registers)', reflected('ps3_add', 'constants') ==
                      'float 5 indexed 0 int 0 bool 0', reflected('ps3_add', 'constants'))
        checks.expect('vs3_skinning_256 binds the whole indexed range',
                      reflected('vs3_skinning_256', 'constants') == 'float 256 indexed 1 int 0 bool 0',
                      reflected('vs3_skinning_256', 'constants'))
        checks.expect('ps3_texld_16_samplers uses 16 2D samplers', reflected('ps3_texld_16_samplers', 'samplers') ==
                      ','.join('%d:2d' % i for i in range(16)), reflected('ps3_texld_16_samplers', 'samplers'))
        checks.expect('ps3_texld_cube and volume types', reflected('ps3_texld_cube', 'samplers') == '0:cube'
                      and reflected('ps3_texld_volume', 'samplers') == '0:3d',
                      f"{reflected('ps3_texld_cube', 'samplers')} {reflected('ps3_texld_volume', 'samplers')}")
        checks.expect('ps3_mrt writes four targets', [o.split()[0] for o in reflected('ps3_mrt', 'output') or []]
                      == ['color0', 'color1', 'color2', 'color3'], str(reflected('ps3_mrt', 'output')))
        checks.expect('pixel shaders read the alpha test block',
                      all('alpha_test' in f['uniforms'] for f, _ in results.values() if f['stage'].startswith('ps')))
        vs_inputs = reflected('vs3_skinning_256', 'input') or []
        checks.expect('vs3_skinning_256 vertex inputs', sorted(i.split()[0] for i in vs_inputs) ==
                      ['blend0', 'position0', 'weight0'], str(vs_inputs))

        # shader model 1.1
        vs11, ps11 = os.path.join(work, 'vs11.bin'), os.path.join(work, 'ps11.bin')
        with open(vs11, 'wb') as f:
            f.write(VS11)
        with open(ps11, 'wb') as f:
            f.write(PS11)
        for name, path, extra in (('vs_1_1', vs11, []), ('ps_1_1', ps11, ['--vertex', vs11])):
            spv = os.path.join(work, name + '.spv')
            code, out, _ = run(binary, ['translate', path, '--out', spv] + extra)
            found = parse(out)
            problem = validate(spv) if code == 0 else out.strip()
            checks.expect(f'{name} translates', code == 0 and not problem, problem)
            if name == 'vs_1_1':
                checks.expect('vs_1_1 reads position0 at attribute 0', found.get('input') == ['position0 location 0 mask xyzw'],
                              str(found.get('input')))
            else:
                checks.expect('ps_1_1 picks the texture type at run time', found.get('samplers') == '0:any',
                              found.get('samplers'))
                checks.expect('ps_1_1 links to vs_1_1 directly', found.get('linked') == 'direct', found.get('linked'))

        # linking
        vs, ps = os.path.join(work, 'link_vs.bin'), os.path.join(work, 'link_ps.bin')
        write(vs, LINK_VS)
        write(ps, LINK_PS)
        alone_spv, linked_spv = os.path.join(work, 'alone.spv'), os.path.join(work, 'linked.spv')
        code_vs, out_vs, _ = run(binary, ['translate', vs])
        code_a, out_a, _ = run(binary, ['translate', ps, '--out', alone_spv])
        code_l, out_l, _ = run(binary, ['translate', ps, '--vertex', vs, '--out', linked_spv])
        print(out_vs + out_l, end='')
        ok = code_vs == 0 and code_a == 0 and code_l == 0
        checks.expect('link shaders translate', ok, out_vs + out_a + out_l)
        if ok:
            vs_out = {o.split()[0]: int(o.split()[2]) for o in parse(out_vs)['output'] if not o.startswith('builtin')}
            ps_in = {i.split()[0]: int(i.split()[2]) for i in parse(out_a)['input'] if not i.startswith('builtin')}
            checks.expect('vertex and pixel shader put binormal0 at different locations',
                          vs_out.get('binormal0') is not None and vs_out.get('binormal0') != ps_in.get('binormal0'),
                          f'vs {vs_out} ps {ps_in}')
            checks.expect('linking was needed', parse(out_l).get('linked') == 'resolved', parse(out_l).get('linked'))
            problem = validate(linked_spv)
            checks.expect('linked pixel shader spirv-val' + (' (skipped)' if spirv_val is None else ''),
                          not problem, problem)
            with open(linked_spv, 'rb') as f:
                locations = Spirv(f.read()).locations(SC_INPUT)
            checks.expect('linked pixel shader reads binormal0 where the vertex shader writes it, and drops texcoord5',
                          locations == [vs_out.get('binormal0')], f'input locations {locations}, vs {vs_out}')
            code, out, _ = run(binary, ['translate', os.path.join(case_dir, 'ps3_add.bin'), '--vertex',
                                        os.path.join(case_dir, 'vs3_arith.bin')])
            checks.expect('fxc-style texcoord pair links directly', code == 0 and parse(out).get('linked') == 'direct',
                          out.strip())

        # float rules: D3D9's strict multiply (both factors checked for zero;
        # dxbc-spirv names its functions for it *_legacy, as dp4_f32_legacy) only with
        # KISAK_VK_STRICT_FLOATS=1; plain multiplies otherwise
        for strict in (False, True):
            env = dict(os.environ)
            env.pop('KISAK_VK_STRICT_FLOATS', None)
            if strict:
                env['KISAK_VK_STRICT_FLOATS'] = '1'
            spv = os.path.join(work, 'floats_%s.spv' % ('strict' if strict else 'fast'))
            code, out, _ = run(binary, ['translate', os.path.join(case_dir, 'ps3_m4x4.bin'), '--out', spv], env=env)
            rule = 'strict' if strict else 'fast'
            legacy = False
            if code == 0:
                with open(spv, 'rb') as f:
                    data = f.read()
                legacy = b'_legacy' in data
            checks.expect(f'floats {rule}: the tool says so, and the legacy multiply is '
                          + ('used' if strict else 'not used'),
                          code == 0 and parse(out).get('floats') == rule and legacy == strict,
                          f'status {code}, floats {parse(out).get("floats")}, legacy multiply {legacy}')

    if checks.failures:
        print(f'{len(checks.failures)} check(s) failed: {", ".join(checks.failures)}')
        return 1
    print('all Legacy shader translator checks passed')
    return 0


if __name__ == '__main__':
    sys.exit(main())
