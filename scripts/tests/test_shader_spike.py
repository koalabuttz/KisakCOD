#!/usr/bin/env python3
"""Asset-free checks for the shader translator spike tools in scripts/tools/shader_spike/.

The shaders here are tiny token streams written by hand for the test, wrapped
in a synthetic fastfile the way Load_MaterialPixelShader reads them.
"""

import json
import os
import struct
import subprocess
import sys
import tempfile
import unittest
import zlib

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.realpath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, os.path.join(REPO, "scripts", "tools", "shader_spike"))

import shader_corpus as sc  # noqa: E402


def ins(opcode, params, control=0, predicated=False):
    token = opcode | (control << 16) | (len(params) << 24)
    if predicated:
        token |= 1 << 28
    return [token] + list(params)


def reg(kind, num, extra=0):
    return 0x80000000 | num | ((kind & 7) << 28) | ((kind & 0x18) << 8) | extra


def dst(kind, num, mod=0):
    return reg(kind, num, (0xF << 16) | (mod << 20))


def src(kind, num, relative=False, srcmod=0):
    return reg(kind, num, (0xE4 << 16) | (srcmod << 24) | ((1 << 13) if relative else 0))


def dcl_io(usage, index, kind, num):
    return ins(31, [0x80000000 | usage | (index << 16), dst(kind, num)])


def dcl_sampler(tex_type, num):
    return ins(31, [0x80000000 | (tex_type << 27), dst(sc.REG_SAMPLER, num)])


def comment_ctab():
    return [0xFFFE | (3 << 16), 0x42415443, 0, 0]


def program(version, *chunks):
    out = [version]
    for c in chunks:
        out += c
    out.append(0x0000FFFF)
    return struct.pack("<%dI" % len(out), *out)


PS3_PREDICATED = program(
    0xFFFF0300,
    comment_ctab(),
    dcl_sampler(2, 0),
    dcl_sampler(3, 1),
    dcl_io(5, 0, sc.REG_INPUT, 0),               # dcl_texcoord0 v0
    ins(31, [0x80000000, dst(sc.REG_MISCTYPE, 0)]),  # dcl vPos
    ins(66, [dst(sc.REG_TEMP, 0), src(sc.REG_INPUT, 0), src(sc.REG_SAMPLER, 0)]),
    ins(66, [dst(sc.REG_TEMP, 1), src(sc.REG_INPUT, 0), src(sc.REG_SAMPLER, 1)], control=1),
    ins(94, [dst(sc.REG_PREDICATE, 0), src(sc.REG_TEMP, 0), src(sc.REG_CONST, 0)], control=1),
    ins(1, [dst(sc.REG_TEMP, 0), src(sc.REG_PREDICATE, 0), src(sc.REG_CONST, 1)], predicated=True),
    ins(1, [dst(sc.REG_COLOROUT, 0, mod=1), src(sc.REG_TEMP, 0)]),
    ins(1, [dst(sc.REG_COLOROUT, 1), src(sc.REG_TEMP, 1, srcmod=11)]),
)

VS3_RELATIVE = program(
    0xFFFE0300,
    ins(81, [dst(sc.REG_CONST, 90), 0, 0, 0, 0x3F800000]),
    dcl_io(0, 0, sc.REG_INPUT, 0),                # dcl_position v0
    dcl_io(0, 0, sc.REG_OUTPUT, 0),               # dcl_position o0
    dcl_io(5, 0, sc.REG_OUTPUT, 1),               # dcl_texcoord0 o1
    ins(46, [dst(sc.REG_ADDR_TEXTURE, 0), src(sc.REG_INPUT, 0)]),
    ins(1, [dst(sc.REG_OUTPUT, 1), src(sc.REG_CONST, 4, relative=True), src(sc.REG_ADDR_TEXTURE, 0)]),
    ins(20, [dst(sc.REG_OUTPUT, 0), src(sc.REG_INPUT, 0), src(sc.REG_CONST, 0)]),
    ins(16, [dst(sc.REG_TEMP, 0), src(sc.REG_CONST, 5)]),
    ins(7, [dst(sc.REG_TEMP, 1), src(sc.REG_TEMP, 0)]),
)

PS2_PLAIN = program(
    0xFFFF0200,
    dcl_sampler(2, 0),
    ins(31, [0x80000000, dst(sc.REG_ADDR_TEXTURE, 0)]),   # dcl t0
    ins(66, [dst(sc.REG_TEMP, 0), src(sc.REG_ADDR_TEXTURE, 0), src(sc.REG_SAMPLER, 0)]),
    ins(1, [dst(sc.REG_COLOROUT, 0), src(sc.REG_TEMP, 0)]),
)


def shader_record(name, code, renderer=1):
    head = struct.pack("<IIIHH", 0xFFFFFFFF, 0, 0xFFFFFFFF, len(code) // 4, renderer)
    return head + name.encode() + b"\x00" + code


def fastfile(path, *records):
    zone = b"\x12\x34" * 10 + b"".join(r + b"\xaa" * 7 for r in records)
    with open(path, "wb") as f:
        f.write(b"IWffu100" + struct.pack("<I", 5) + zlib.compress(zone))


class ParseTests(unittest.TestCase):
    def test_ps3_features(self):
        info = sc.parse_bytecode(PS3_PREDICATED)
        self.assertEqual(info["model"], "ps_3_0")
        self.assertEqual(info["ops"]["texld"], 1)
        self.assertEqual(info["ops"]["texldp"], 1)
        self.assertEqual(info["samplers"], {"0": "2d", "1": "cube"})
        for feat in ("ctab", "predicated_instruction", "vpos", "mrt", "saturate", "srcmod_abs"):
            self.assertIn(feat, info["features"])
        verdict = sc.predict(info)
        self.assertEqual(verdict["vkd3d-shader"]["reject"], ["op_setp", "vpos_vface_with_inputs"])
        self.assertIn("predicated_instruction", verdict["mojoshader"]["reject"])
        self.assertIn("texldp_cube", verdict["mojoshader"]["reject"])
        self.assertEqual(verdict["dxbc-spirv"]["reject"], [])

    def test_vs3_relative(self):
        info = sc.parse_bytecode(VS3_RELATIVE)
        self.assertEqual(info["model"], "vs_3_0")
        self.assertIn("relative_src_const", info["features"])
        self.assertIn("relative_via_a0", info["features"])
        self.assertIn("def_float", info["features"])
        self.assertEqual(info["max_float_const"], 90)
        self.assertEqual(info["outputs"], 2)
        verdict = sc.predict(info)
        self.assertEqual(verdict["vkd3d-shader"]["reject"], ["op_lit"])
        self.assertIn("rsq_log_without_abs", verdict["vkd3d-shader"]["risk"])
        self.assertIn("no_d3d9_zero_times_inf", verdict["mojoshader"]["risk"])
        self.assertEqual(verdict["mojoshader"]["reject"], [])
        self.assertIn("relative_const_with_def", verdict["mojoshader"]["risk"])

    def test_ps2(self):
        info = sc.parse_bytecode(PS2_PLAIN)
        self.assertEqual(info["model"], "ps_2_0")
        self.assertEqual(info["inputs"], 1)
        self.assertEqual(info["instructions"], 2)

    def test_rejects_bad_streams(self):
        with self.assertRaises(sc.BytecodeError):
            sc.parse_bytecode(PS2_PLAIN[:-4])
        with self.assertRaises(sc.BytecodeError):
            sc.parse_bytecode(struct.pack("<II", 0x12345678, 0xFFFF))
        with self.assertRaises(sc.BytecodeError):
            sc.parse_bytecode(struct.pack("<III", 0xFFFF0300, 0x0000007F, 0xFFFF))

    def test_families(self):
        self.assertEqual(sc.classify_family("lm_r0c0_sm"), "world_lit")
        self.assertEqual(sc.classify_family("postfx_glow"), "postfx")
        self.assertEqual(sc.classify_family("zzz"), "other")


class FastfileTests(unittest.TestCase):
    def test_scan_and_dedup(self):
        with tempfile.TemporaryDirectory() as tmp:
            fastfile(os.path.join(tmp, "common_mp.ff"),
                     shader_record("postfx_glow", PS2_PLAIN, renderer=0),
                     shader_record("lm_test", PS3_PREDICATED),
                     # truncated program: no end token where the size says
                     shader_record("broken", PS2_PLAIN[:-8] + b"\0\0\0\0\0\0\0\0"))
            fastfile(os.path.join(tmp, "killhouse.ff"),
                     shader_record("lm_test", PS3_PREDICATED),
                     shader_record("sky_x", VS3_RELATIVE))
            with open(os.path.join(tmp, "not_a_zone.ff"), "wb") as f:
                f.write(b"nope")
            dump = os.path.join(tmp, "dump")
            report = sc.to_jsonable(sc.build_report(sc.resolve_groups([
                "mp=%s/common_mp.ff" % tmp, "sp=%s/*.ff" % tmp]), dump))
            self.assertEqual(report["unique_shaders"], 3)
            self.assertEqual(report["by_model"], {"ps_2_0": 1, "ps_3_0": 1, "vs_3_0": 1})
            self.assertEqual(report["by_group_only"], {"mp": 1, "mp+sp": 1, "sp": 1})
            self.assertEqual(report["by_renderer"], {"renderer_1": 2, "renderer_0": 1})
            zones = {z["zone"]: z for z in report["zones"]}
            self.assertEqual(zones["common_mp.ff"]["shaders"], 2)
            self.assertEqual(zones["killhouse.ff"]["unique_new"], 1)
            self.assertIn("error", zones["not_a_zone.ff"])
            self.assertEqual(report["predicted"]["vkd3d-shader"]["reject"], 2)
            self.assertEqual(report["predicted"]["dxbc-spirv"]["accept"], 3)
            self.assertEqual(report["predicted"]["mojoshader"]["reject"], 1)
            self.assertEqual(len(os.listdir(dump)), 3)

    def test_cli_refuses_dump_in_repo(self):
        with tempfile.TemporaryDirectory() as tmp:
            fastfile(os.path.join(tmp, "a.ff"), shader_record("x", PS2_PLAIN))
            cmd = [sys.executable, os.path.join(REPO, "scripts", "tools", "shader_spike", "shader_corpus.py"),
                   "--group", "all=%s/*.ff" % tmp]
            bad = subprocess.run(cmd + ["--dump-dir", os.path.join(REPO, "build-shaders")],
                                 capture_output=True, text=True)
            self.assertNotEqual(bad.returncode, 0)
            self.assertIn("outside the repository", bad.stderr)
            good = subprocess.run(cmd + ["--no-prefixes"], capture_output=True, text=True, check=True)
            report = json.loads(good.stdout)
            self.assertEqual(report["unique_shaders"], 1)
            self.assertNotIn("by_prefix", report)


class AssemblerAndCaseTests(unittest.TestCase):
    """The spike's assembler and op cases, checked against the scanner's own parser."""

    def test_every_case_assembles_and_parses(self):
        import d3d9_asm
        import cases as shader_spike_cases
        seen_ops = set()
        for name, source in shader_spike_cases.all_cases().items():
            info = sc.parse_bytecode(d3d9_asm.assemble(source))
            self.assertIn("ctab", info["features"], name)
            self.assertTrue(info["model"].startswith(name[:2]), name)
            seen_ops.update(info["ops"])
        sm2_sm3_ops = {"add", "sub", "mul", "mad", "rcp", "rsq", "dp3", "dp4", "min", "max",
                       "slt", "sge", "exp", "log", "lit", "dst", "lrp", "frc", "pow", "crs",
                       "sgn", "abs", "nrm", "sincos", "cmp", "dp2add", "dsx", "dsy", "mova",
                       "texld", "texldp", "texldb", "texldl", "texldd", "texkill", "setp",
                       "if", "ifc", "else", "loop", "rep", "break", "breakc", "breakp",
                       "call", "callnz", "label", "ret", "expp", "logp", "m4x4", "m4x3",
                       "m3x4", "m3x3", "m3x2", "def", "defi", "defb"}
        self.assertEqual(sm2_sm3_ops - seen_ops, set())

    def test_encoding_details(self):
        import d3d9_asm
        code = d3d9_asm.assemble("ps_3_0\ndcl_texcoord0 v0\nsetp_gt p0, v0, c1\n(p0) mov r0, c[aL + 2]\nmov oC0, r0", ctab=False)
        tokens = struct.unpack("<%dI" % (len(code) // 4), code)
        self.assertEqual(tokens[0], 0xFFFF0300)
        setp = tokens[4]
        self.assertEqual(setp & 0xFFFF, 94)
        self.assertEqual((setp >> 16) & 0xFF, 1)            # _gt
        mov = tokens[8]
        self.assertTrue(mov & (1 << 28))                     # predicated
        self.assertEqual((mov >> 24) & 0xF, 4)               # dst, p0, c[aL+2], aL
        self.assertEqual(sc._reg_type(tokens[10]), sc.REG_PREDICATE)   # predicate follows dst
        self.assertTrue(tokens[11] & (1 << 13))
        self.assertEqual(tokens[11] & 0x7FF, 2)
        self.assertEqual(sc._reg_type(tokens[12]), sc.REG_LOOP)
        with self.assertRaises(d3d9_asm.AsmError):
            d3d9_asm.assemble("ps_3_0\nfrobnicate r0, r1")


class RunnerTests(unittest.TestCase):
    def test_runner_with_stand_in_translators(self):
        import run_translators as shader_spike_run
        with tempfile.TemporaryDirectory() as tmp:
            # Stand-ins: one copies a fixed SPIR-V header, one always fails.
            good = os.path.join(tmp, "good.py")
            with open(good, "w") as f:
                f.write("import sys, struct\n"
                        "open(sys.argv[2], 'wb').write(struct.pack('<5I', 0x07230203, 0x10000, 0, 1, 0))\n")
            bad = os.path.join(tmp, "bad.py")
            with open(bad, "w") as f:
                f.write("import sys\nsys.stderr.write('nope: unsupported\\n')\nsys.exit(1)\n")
            for p in (good, bad):
                os.chmod(p, 0o755)
            val = os.path.join(tmp, "val.py")
            with open(val, "w") as f:
                f.write("import sys\nsys.exit(0)\n")
            os.chmod(val, 0o755)
            wrap = lambda p: os.path.join(tmp, os.path.basename(p) + ".sh")
            for p in (good, bad, val):
                with open(wrap(p), "w") as f:
                    f.write("#!/bin/sh\nexec %s %s \"$@\"\n" % (sys.executable, p))
                os.chmod(wrap(p), 0o755)
            out = os.path.join(tmp, "r.json")
            shader_spike_run.main(["--mojoshader", wrap(good), "--vkd3d", wrap(bad),
                                   "--spirv-val", wrap(val), "--out", out])
            with open(out) as f:
                report = json.load(f)
            n = report["shaders"]
            self.assertGreater(n, 50)
            self.assertEqual(report["translators"]["mojoshader"]["status"], {"ok": n})
            self.assertEqual(report["translators"]["vkd3d-shader"]["status"], {"rejected": n})
            self.assertEqual(report["translators"]["vkd3d-shader"]["failure_reasons"],
                             {"nope: unsupported": n})


if __name__ == "__main__":
    unittest.main()
