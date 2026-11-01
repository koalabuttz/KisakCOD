#!/usr/bin/env python3
"""Synthetic world surface preparation checks; no game assets, no packaged bytes.

Every archive and payload here is built in a temporary directory, so these tests
never read an installation and never decode a real texture or shader.
"""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
import zipfile
sys.path.insert(0, str(Path(__file__).parents[1] / 'tools'))
import prepare_linux_world_surface as module
from prepare_linux_world_surface import (EXPORT_NAME, MANIFEST_NAME, check_shader,
                                         prepare, sha256, verify_prepared_manifest)

CAPS = {'max_bundle_bytes': 67108864, 'max_shader_bytes': 262144,
        'max_bundle_vertices': 65535, 'max_bundle_indices': 1048576}
MOV = 0x0001
END = 0x0000FFFF
COMMENT = 0x0000FFFE


def fnv(data):
    digest = 0xcbf29ce484222325
    for byte in data:
        digest = ((digest ^ byte) * 0x100000001b3) % (1 << 64)
    return '%016x' % digest


def pack(values):
    return struct.pack('<%dI' % len(values), *values)


def instruction(opcode, operands):
    """One instruction: its length field counts the DWORDs after the instruction token."""
    return ((len(operands) << 24) | opcode, *operands)


def comment(payload):
    return ((len(payload) << 16) | COMMENT, *payload)


def shader_blob(stage, body=None, end=True):
    version = 0xFFFF0300 if stage == 'pixel' else 0xFFFE0300
    parts = [instruction(MOV, (0x800F0000, 0x90E40000))] if body is None else body
    values = [version]
    for part in parts:
        values.extend(part)
    if end:
        values.append(END)
    return pack(values)


def texture_bytes(name):
    return b'IWi' + bytes([6, 11, 0]) + name.encode().ljust(32, b'\0') + \
        bytes((index * 37 + 11) % 256 for index in range(512))


def texture_entries(names=('me_concrete_dest02_col', 'me_concrete_dest02_nml')):
    return [('images/%s.iwi' % name, texture_bytes(name)) for name in names]


def write_archive(path, entries, compression=zipfile.ZIP_DEFLATED):
    path.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(path, 'w', compression) as archive:
        for name, data in entries:
            archive.writestr(name, data)
    return path


def write_bundle(root, vertex_stride=44, vertex_count=3, index_count=3,
                 textures=('me_concrete_dest02_col', 'me_concrete_dest02_nml'), vs=None, ps=None,
                 caps=None, payload_delta=0):
    root = Path(root)
    root.mkdir(parents=True, exist_ok=True)
    vs = shader_blob('vertex') if vs is None else vs
    ps = shader_blob('pixel') if ps is None else ps
    vertices = bytes((index * 7 + 3) % 256 for index in range(vertex_stride * vertex_count))
    indices = struct.pack('<%dH' % index_count, *range(index_count))
    payload = {'world-surface-vs.bin': vs, 'world-surface-ps.bin': ps,
               'world-surface-vertices.bin': vertices, 'world-surface-indices.bin': indices}
    for name, data in payload.items():
        (root / name).write_bytes(data)
    roles = {'vertex_shader_bytecode': ('world-surface-vs.bin', vs, len(vs) // 4),
             'pixel_shader_bytecode': ('world-surface-ps.bin', ps, len(ps) // 4),
             'world_vertices': ('world-surface-vertices.bin', vertices, vertex_count),
             'world_indices': ('world-surface-indices.bin', indices, index_count)}
    files = [{'role': role, 'name': name, 'bytes': len(data), 'fnv1a64': fnv(data), 'sha256': None}
             for role, (name, data, _) in roles.items()]
    shaders = {role: {'role': role, 'name': 'shader.hlsl', 'word_count': len(data) // 4,
                      'byte_count': len(data), 'load_for_renderer': 1, 'file': name,
                      'fnv1a64': fnv(data), 'sha256': None}
               for role, (name, data, _) in (('vertex', roles['vertex_shader_bytecode']),
                                             ('pixel', roles['pixel_shader_bytecode']))}
    dependencies = [{'slot': slot, 'semantic': 5 if 'nml' in name else 2, 'name_hash': slot + 1,
                     'sampler_state': 19, 'sampler_state_decoded': False, 'image_name': name,
                     'iwi_entry': 'images/%s.iwi' % name, 'payload': 'external',
                     'pixels_written': False,
                     'image': {'name': name, 'map_type': 3, 'width': 512, 'height': 256}}
                    for slot, name in enumerate(textures)]
    document = {
        'schema_version': 1, 'document': 'kisak_static_world_surface_bundle',
        'tool': 'offline static world surface bundle export',
        'render_ready': False, 'replay_claimed': False, 'notes': ['synthetic fixture'],
        'request': {'map': 'mp_test', 'surface_index': 3, 'selection_source': 'KISAK_WORLD_SURFACE_INDEX',
                    'zone_alloc_flags': 8, 'zone_free_flags': 8},
        'world': {'name': 'maps/mp/mp_test.d3dbsp', 'checksum': 0},
        'surface': {'index': 3, 'flags': 1, 'lightmap_index': 0},
        'geometry': {'first_vertex': 0, 'vertex_count': vertex_count, 'tri_count': index_count // 3,
                     'base_index': 0, 'index_count': index_count, 'vertex_stride': vertex_stride,
                     'index_space': 'surface_local'},
        'slice': {'vertex_file': 'world-surface-vertices.bin', 'vertex_bytes': len(vertices),
                  'vertex_stride': vertex_stride, 'index_file': 'world-surface-indices.bin',
                  'index_bytes': len(indices), 'index_stride': 2},
        'material': {'name': 'wc/test', 'state_flags': 57, 'texture_count': len(textures)},
        'technique': {'name': 'vcsh', 'type': 4, 'pass_count': 1},
        'declaration': {'declaration_type': 2, 'name': 'plain_world', 'stream_count': 3,
                        'complete': True},
        'pass': {'index': 0, 'state': {'state_bits_index': 1, 'load_bits': [403867666, 13]},
                 'arguments': {'per_prim': [], 'per_object': [], 'stable': []},
                 'shaders': shaders},
        'texture_dependencies': dependencies, 'files': files,
        'bytes': {'vertex_shader': len(vs), 'pixel_shader': len(ps),
                  'vertices': len(vertices), 'indices': len(indices),
                  'payload_total': sum(len(data) for data in payload.values()) + payload_delta},
        'caps': dict(CAPS if caps is None else caps),
        'unresolved': [{'reason': 'code_vertex_constant_requires_scene'},
                       {'reason': 'material_texture_payload_not_captured'},
                       {'reason': 'texture_payload_not_captured'}],
    }
    path = root / 'world-surface-bundle.json'
    path.write_text(json.dumps(document))
    return path


def run(root, entries=None, bundle_overrides=None, pins=None, archives=('iw_02.iwd',)):
    root = Path(root)
    bundle = write_bundle(root / 'export', **(bundle_overrides or {}))
    for name in archives:
        write_archive(root / 'game' / 'main' / name, texture_entries() if entries is None else entries)
    out = root / 'prepared'
    manifest, digest = prepare(bundle, root / 'game', out, pins)
    return bundle, out, manifest, digest


class NoReadMixin:
    """Records opens so a test can prove one specific file was never read."""

    def no_read(self, action, forbidden):
        original = module.open_readonly
        opened = []

        def record(path):
            opened.append(str(path))
            return original(path)

        module.open_readonly = record
        try:
            with self.assertRaises(ValueError) as caught:
                action()
        finally:
            module.open_readonly = original
        self.assertEqual([path for path in opened if str(forbidden) in path], [],
                         'the payload was opened before the cap was enforced')
        return str(caught.exception)


class Tests(NoReadMixin, unittest.TestCase):
    def test_prepares_exact_copies_with_sha256_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle, out, manifest, digest = run(temporary)
            source = bundle.parent
            self.assertEqual(oct(out.stat().st_mode & 0o777), oct(0o700))
            for artifact in manifest['artifacts']:
                copied = (out / artifact['file']).read_bytes()
                self.assertEqual(copied, (source / artifact['file']).read_bytes())
                self.assertEqual(artifact['sha256'], hashlib.sha256(copied).hexdigest())
                self.assertEqual(oct((out / artifact['file']).stat().st_mode & 0o777), oct(0o600))
            for texture in manifest['textures']:
                copied = (out / texture['file']).read_bytes()
                self.assertEqual(copied, texture_bytes(texture['dependency']['image_name']))
                self.assertEqual(texture['sha256'], hashlib.sha256(copied).hexdigest())
                self.assertEqual(texture['provenance']['archive'], 'iw_02.iwd')
                self.assertFalse(texture['decoded'])
            self.assertEqual((out / MANIFEST_NAME).read_bytes(), json.dumps(manifest, indent=2).encode() + b'\n')
            self.assertEqual(digest, hashlib.sha256((out / MANIFEST_NAME).read_bytes()).hexdigest())
            self.assertEqual(sorted(path.name for path in out.iterdir()),
                             sorted([artifact['file'] for artifact in manifest['artifacts']]
                                    + ['images', EXPORT_NAME, MANIFEST_NAME]))

    def test_export_sibling_is_verbatim(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle, out, manifest, _ = run(temporary)
            sibling = (out / EXPORT_NAME).read_bytes()
            self.assertEqual(sibling, bundle.read_bytes())
            self.assertEqual(manifest['source']['sha256'], sha256(sibling))
            self.assertEqual(manifest['source']['export_file'], EXPORT_NAME)
            self.assertEqual(manifest['export'], json.loads(sibling))
            self.assertTrue(all(item['sha256'] is None for item in manifest['export']['files']))

    def test_preserves_export_request_state_and_declaration(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle, _, manifest, _ = run(temporary)
            document = json.loads(bundle.read_text())
            self.assertEqual(manifest['export'], document)
            for field in ('request', 'surface', 'geometry', 'slice', 'material', 'technique',
                          'declaration', 'pass', 'caps', 'bytes'):
                self.assertEqual(manifest[field], document[field], field)
            self.assertFalse(manifest['render_ready'])
            self.assertFalse(manifest['replay_claimed'])
            self.assertFalse(manifest['shader_semantics_validated'])
            self.assertEqual(manifest['trusted_caps'], CAPS)

    def test_trusted_caps_cannot_be_raised_by_the_document(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, _, manifest, _ = run(temporary, bundle_overrides={
                'caps': {'max_bundle_bytes': 8 << 30, 'max_shader_bytes': 4 << 20,
                         'max_bundle_vertices': 1 << 20, 'max_bundle_indices': 1 << 22}})
            self.assertEqual(manifest['trusted_caps'], CAPS)
            self.assertEqual(manifest['caps']['max_bundle_bytes'], 8 << 30)

    def test_sparse_oversize_payload_is_refused_without_a_read(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            huge = 1 << 30
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            document = json.loads(bundle.read_text())
            record = next(item for item in document['files'] if item['role'] == 'world_vertices')
            record['bytes'] = huge
            document['bytes']['payload_total'] += huge - (root / 'export' / record['name']).stat().st_size
            bundle.write_text(json.dumps(document))
            with open(root / 'export' / record['name'], 'wb') as stream:
                stream.truncate(huge)
            message = self.no_read(lambda: prepare(bundle, root / 'game', root / 'prepared'),
                                   root / 'export' / record['name'])
            self.assertIn('budget', message)

    def test_inflated_document_cap_still_hits_the_trusted_cap(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            huge = 70 << 20
            bundle = write_bundle(root / 'export', caps={'max_bundle_bytes': 8 << 30,
                                                         'max_shader_bytes': 4 << 20,
                                                         'max_bundle_vertices': 1 << 20,
                                                         'max_bundle_indices': 1 << 22})
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            document = json.loads(bundle.read_text())
            record = next(item for item in document['files'] if item['role'] == 'world_vertices')
            record['bytes'] = huge
            document['bytes']['payload_total'] += huge - (root / 'export' / record['name']).stat().st_size
            bundle.write_text(json.dumps(document))
            with open(root / 'export' / record['name'], 'wb') as stream:
                stream.truncate(huge)
            message = self.no_read(lambda: prepare(bundle, root / 'game', root / 'prepared'),
                                   root / 'export' / record['name'])
            self.assertIn('budget', message)

    def test_shader_over_the_trusted_cap_is_refused_without_a_read(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            oversized = 300 * 1024
            bundle = write_bundle(root / 'export', caps={'max_bundle_bytes': 67108864,
                                                         'max_shader_bytes': 4 << 20,
                                                         'max_bundle_vertices': 65535,
                                                         'max_bundle_indices': 1048576})
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            document = json.loads(bundle.read_text())
            record = next(item for item in document['files'] if item['role'] == 'vertex_shader_bytecode')
            record['bytes'] = oversized
            document['bytes']['payload_total'] += oversized - (root / 'export' / record['name']).stat().st_size
            document['pass']['shaders']['vertex']['byte_count'] = oversized
            document['pass']['shaders']['vertex']['word_count'] = oversized // 4
            bundle.write_text(json.dumps(document))
            with open(root / 'export' / record['name'], 'wb') as stream:
                stream.truncate(oversized)
            message = self.no_read(lambda: prepare(bundle, root / 'game', root / 'prepared'),
                                   root / 'export' / record['name'])
            self.assertIn('shader cap', message)

    def test_rejects_payload_digest_length_and_total_tampering(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            vertices = root / 'export' / 'world-surface-vertices.bin'
            original = vertices.read_bytes()
            vertices.write_bytes(bytes([original[0] ^ 0xFF]) + original[1:])
            with self.assertRaisesRegex(ValueError, 'digest'):
                prepare(bundle, root / 'game', root / 'one')
            vertices.write_bytes(original[:-4])
            with self.assertRaisesRegex(ValueError, 'length'):
                prepare(bundle, root / 'game', root / 'two')
            self.assertFalse((root / 'one').exists())
            self.assertFalse((root / 'two').exists())
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export', payload_delta=1)
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            with self.assertRaisesRegex(ValueError, 'payload total'):
                prepare(bundle, root / 'game', root / 'three')

    def test_rejects_unplain_payload_names(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            document = json.loads(bundle.read_text())
            for name in ('../escape.bin', 'nested/file.bin', '.', '/absolute.bin', 'windows\\path.bin'):
                document['files'][0]['name'] = name
                bundle.write_text(json.dumps(document))
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'plain file name'):
                    prepare(bundle, root / 'game', root / 'out')

    def test_requires_every_declared_texture(self):
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, 'no authorized archive provides'):
                run(temporary, entries=texture_entries(('me_concrete_dest02_col',)))
        with tempfile.TemporaryDirectory() as temporary:
            _, _, manifest, _ = run(temporary, entries=texture_entries(('me_concrete_dest02_col',)),
                                    bundle_overrides={'textures': ('me_concrete_dest02_col',)})
            self.assertEqual([texture['iwi_entry'] for texture in manifest['textures']],
                             ['images/me_concrete_dest02_col.iwi'])

    def test_unknown_entries_are_ignored(self):
        with tempfile.TemporaryDirectory() as temporary:
            entries = texture_entries() + [('images/unrelated.iwi', b'not needed'),
                                           ('misc/readme.txt', b'ignored'),
                                           ('images/', b''), ('sound/ambient/', b'')]
            _, out, _, _ = run(temporary, entries=entries)
            self.assertEqual(sorted(path.name for path in (out / 'images').iterdir()),
                             ['me_concrete_dest02_col.iwi', 'me_concrete_dest02_nml.iwi'])

    def test_duplicate_entries_must_be_identical(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            for name in ('iw_01.iwd', 'iw_02.iwd'):
                write_archive(root / 'game' / 'main' / name, texture_entries())
            manifest, _ = prepare(bundle, root / 'game', root / 'prepared')
            self.assertEqual(len(manifest['textures'][0]['provenance']['copies']), 2)
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_01.iwd', texture_entries())
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            write_archive(root / 'game' / 'main' / 'iw_01.iwd',
                          [('images/me_concrete_dest02_col.iwi', b'IWi different bytes')])
            with self.assertRaisesRegex(ValueError, 'disagree'):
                prepare(bundle, root / 'game', root / 'prepared')

    def test_rejects_escaping_entry_names(self):
        for name in ('../escape.iwi', 'images/../../escape.iwi', '/absolute/escape.iwi',
                     'images/./escape.iwi', 'C:/escape.iwi', 'images//escape.iwi'):
            with tempfile.TemporaryDirectory() as temporary:
                with self.subTest(name=name), self.assertRaisesRegex(ValueError, 'escapes|not relative'):
                    run(temporary, entries=texture_entries() + [(name, b'escape')])

    def test_rejects_malformed_archives(self):
        cases = [('truncate', lambda path: path.write_bytes(path.read_bytes()[:path.stat().st_size // 2]),
                  'end-of-central-directory'),
                 ('end-record', lambda path: patch(path, path.stat().st_size - 22, b'PK\x05\x07'),
                  'end-of-central-directory'),
                 ('central-signature', lambda path: patch(path, central_offset(path), b'PK\x01\x03'),
                  'central directory signature'),
                 ('entry-crc', lambda path: patch(path, central_offset(path) + 16,
                                                  struct.pack('<I', 0xDEADBEEF)), 'CRC-32'),
                 ('local-signature', lambda path: patch(path, local_offset(path), b'PK\x03\x05'),
                  'local header signature'),
                 ('zip64-sentinel', lambda path: announce_zip64(path), 'unsupported'),
                 ('compression-method', lambda path: patch(path, central_offset(path) + 10,
                                                           struct.pack('<H', 12)), 'compression method'),
                 ('declared-size', lambda path: patch(path, central_offset(path) + 24,
                                                      struct.pack('<I', 3)), 'declared size')]
        for label, mutate, message in cases:
            with tempfile.TemporaryDirectory() as temporary:
                root = Path(temporary)
                bundle = write_bundle(root / 'export')
                archive = write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
                mutate(archive)
                with self.subTest(case=label), self.assertRaisesRegex(ValueError, message):
                    prepare(bundle, root / 'game', root / 'prepared')
                self.assertFalse((root / 'prepared').exists())

    def test_bounds_entry_size_and_compression_ratio(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_02.iwd',
                          [('images/me_concrete_dest02_col.iwi', b'\0' * (4 * 1024 * 1024)),
                           ('images/me_concrete_dest02_nml.iwi', texture_bytes('me_concrete_dest02_nml'))])
            with self.assertRaisesRegex(ValueError, 'compression ratio'):
                prepare(bundle, root / 'game', root / 'prepared')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            archive = write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            patch(archive, central_offset(archive) + 20, struct.pack('<I', 1024 * 1024))
            patch(archive, central_offset(archive) + 24, struct.pack('<I', 40 * 1024 * 1024))
            with self.assertRaisesRegex(ValueError, 'more bytes than allowed'):
                prepare(bundle, root / 'game', root / 'prepared')

    def test_output_directory_must_be_new_private_and_inside_no_game_tree(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            (root / 'prepared').mkdir()
            with self.assertRaisesRegex(ValueError, 'must not exist'):
                prepare(bundle, root / 'game', root / 'prepared')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            (root / 'real').mkdir()
            (root / 'linked').symlink_to(root / 'real')
            with self.assertRaisesRegex(ValueError, 'symbolic link|must not exist'):
                prepare(bundle, root / 'game', root / 'linked' / 'prepared')
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            with self.assertRaisesRegex(ValueError, 'inside the game tree'):
                prepare(bundle, root / 'game', root / 'game' / 'prepared')

    def test_never_edits_the_authorized_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            archive = write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            before = archive.read_bytes()
            prepare(bundle, root / 'game', root / 'prepared')
            self.assertEqual(archive.read_bytes(), before)

    def test_stored_entries_are_supported(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, _, manifest, _ = run(temporary, entries=texture_entries(), archives=['iw_00.iwd'])
            self.assertEqual(manifest['textures'][0]['provenance']['archive'], 'iw_00.iwd')

    def test_shader_framing_accepts_well_formed_3_0(self):
        for stage in ('vertex', 'pixel'):
            shader = check_shader(shader_blob(stage), stage, CAPS)
            self.assertEqual(shader['structural_status'], 'validated')
            self.assertEqual(shader['instruction_count'], 1)
            self.assertEqual(shader['comment_count'], 0)
            self.assertEqual(shader['trailing_words'], 0)
            self.assertFalse(shader['semantics_validated'])

    def test_shader_framing_counts_instruction_length_after_its_token(self):
        # A MOV with two operands must consume four DWORDs, not one.
        blob = shader_blob('vertex')
        self.assertEqual(len(blob) // 4, 5)
        self.assertEqual(check_shader(blob, 'vertex', CAPS)['end_offset'], 4)

    def test_shader_framing_accepts_end_immediately_after_the_version(self):
        for stage in ('vertex', 'pixel'):
            shader = check_shader(pack((0xFFFF0300 if stage == 'pixel' else 0xFFFE0300, END)), stage, CAPS)
            self.assertEqual(shader['instruction_count'], 0)
            self.assertEqual(shader['end_offset'], 1)

    def test_shader_framing_does_not_stop_at_an_operand_that_looks_like_end(self):
        blob = shader_blob('vertex', body=[instruction(MOV, (END, 0x00000000))])
        shader = check_shader(blob, 'vertex', CAPS)
        self.assertEqual(shader['instruction_count'], 1)
        self.assertEqual(shader['end_offset'], len(blob) // 4 - 1)

    def test_shader_framing_accepts_nop_with_zero_length(self):
        shader = check_shader(shader_blob('vertex', body=[(0x00000000,)]), 'vertex', CAPS)
        self.assertEqual(shader['instruction_count'], 1)

    def test_shader_framing_comment_lengths(self):
        valid_one = shader_blob('vertex', body=[comment((0x12345678,))])
        self.assertEqual(check_shader(valid_one, 'vertex', CAPS)['comment_count'], 1)
        # 300 DWORDs cannot fit in 8 bits, so a truncated comment field desynchronises here.
        wide = shader_blob('vertex', body=[comment(tuple(range(300)))])
        self.assertEqual(check_shader(wide, 'vertex', CAPS)['comment_count'], 1)
        self.assertEqual(check_shader(wide, 'vertex', CAPS)['end_offset'], len(wide) // 4 - 1)

    def test_shader_framing_rejects_malformed_streams(self):
        cases = [('unknown-version', shader_blob('vertex').replace(b'\x00\x03\xfe\xff', b'\x00\x02\xfe\xff', 1),
                  'vertex', 'not the supported'),
                 ('wrong-stage', shader_blob('pixel'), 'vertex', 'not the supported'),
                 ('missing-end', shader_blob('vertex', end=False), 'vertex', 'ends without an end token'),
                 ('trailing-word', shader_blob('vertex', body=[instruction(MOV, (0x800F0000, 0x90E40000)),
                                                               (END,), (0x02000001,)], end=False),
                  'vertex', 'DWORDs follow it'),
                 ('comment-zero', shader_blob('vertex', body=[(COMMENT,)]), 'vertex', 'zero length'),
                 ('comment-truncated', shader_blob('vertex', body=[((5 << 16) | COMMENT, 1)]),
                  'vertex', 'overruns the bytecode'),
                 ('instruction-truncated', shader_blob('vertex', body=[((2 << 24) | MOV, 0x800F0000)], end=False),
                  'vertex', 'overruns the bytecode'),
                 ('instruction-zero-length', shader_blob('vertex', body=[(MOV,)]),
                  'vertex', 'declares zero length'),
                 ('not-whole-words', shader_blob('vertex') + b'\x01', 'vertex', 'whole number of words')]
        for label, blob, stage, message in cases:
            with self.subTest(case=label), self.assertRaisesRegex(ValueError, message):
                check_shader(blob, stage, CAPS)

    def test_shader_rejects_payload_over_its_cap(self):
        with self.assertRaisesRegex(ValueError, 'shader cap'):
            check_shader(shader_blob('vertex'), 'vertex', dict(CAPS, max_shader_bytes=8))

    def test_manifest_states_the_framing_and_keeps_the_context_unresolved(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, _, manifest, _ = run(temporary)
            for role in ('vertex', 'pixel'):
                shader = manifest['shader_checks'][role]
                self.assertEqual(shader['structural_status'], 'validated')
                self.assertFalse(shader['semantics_validated'])
                self.assertEqual(shader['version'], '0xffff0300' if role == 'pixel' else '0xfffe0300')
            self.assertTrue(manifest['replay']['structural_eligible'])
            self.assertFalse(manifest['replay']['reject_before_create'])
            self.assertIsNone(manifest['replay']['format'])
            self.assertIn('code_vertex_constant_requires_scene', manifest['replay']['context_unresolved'])
            self.assertFalse(manifest['render_ready'])
            self.assertFalse(manifest['shader_semantics_validated'])

    def test_texture_pins_are_enforced(self):
        with tempfile.TemporaryDirectory() as temporary:
            data = texture_bytes('me_concrete_dest02_col')
            run(temporary, pins={'images/me_concrete_dest02_col.iwi': (sha256(data), len(data))})
        for digest, size in ((('0' * 64), len(texture_bytes('me_concrete_dest02_col'))),
                             (sha256(texture_bytes('me_concrete_dest02_col')), 1)):
            with tempfile.TemporaryDirectory() as temporary:
                with self.subTest(size=size), self.assertRaisesRegex(ValueError, 'pinned bytes'):
                    run(temporary, pins={'images/me_concrete_dest02_col.iwi': (digest, size)})
        with tempfile.TemporaryDirectory() as temporary:
            with self.assertRaisesRegex(ValueError, 'does not depend on'):
                run(temporary, pins={'images/absent.iwi': ('0' * 64, 1)})

    def test_manifest_records_what_this_step_resolved(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, _, manifest, _ = run(temporary)
            reasons = [item['reason'] for item in manifest['resolved']]
            self.assertIn('texture_payload_not_captured', reasons)
            self.assertIn('material_texture_payload_not_captured', reasons)
            self.assertEqual([item['reason'] for item in manifest['unresolved']],
                             ['code_vertex_constant_requires_scene',
                              'material_texture_payload_not_captured',
                              'texture_payload_not_captured'])

    def test_prepare_budget_covers_every_texture_before_it_is_read(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            block = 24 * 1024 * 1024
            names = ('budget_a', 'budget_b', 'budget_c')
            bundle = write_bundle(root / 'export', textures=names)
            write_archive(root / 'game' / 'main' / 'iw_02.iwd',
                          [('images/%s.iwi' % name, bytes(block)) for name in names],
                          compression=zipfile.ZIP_STORED)
            original = module.extract_entry
            seen = []

            def spy(archive, entry):
                seen.append(entry['name'])
                return original(archive, entry)

            module.extract_entry = spy
            try:
                with self.assertRaisesRegex(ValueError, 'prepared budget'):
                    prepare(bundle, root / 'game', root / 'prepared')
            finally:
                module.extract_entry = original
            self.assertEqual(seen, ['images/budget_a.iwi', 'images/budget_b.iwi'])
            self.assertFalse((root / 'prepared').exists())

    def test_repeated_dependencies_are_extracted_once(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export', textures=('me_concrete_dest02_col',) * 5)
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            original = module.extract_entry
            seen = []

            def spy(archive, entry):
                seen.append(entry['name'])
                return original(archive, entry)

            module.extract_entry = spy
            try:
                manifest, _ = prepare(bundle, root / 'game', root / 'prepared')
            finally:
                module.extract_entry = original
            self.assertEqual(seen, ['images/me_concrete_dest02_col.iwi'])
            self.assertEqual(len(manifest['textures']), 5)
            self.assertEqual(len({texture['sha256'] for texture in manifest['textures']}), 1)
            self.assertEqual(verify_prepared_manifest(root / 'prepared')['texture_dependencies'],
                             json.loads(bundle.read_text())['texture_dependencies'])

    def test_too_many_dependencies_are_rejected_before_extraction(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            bundle = write_bundle(root / 'export')
            write_archive(root / 'game' / 'main' / 'iw_02.iwd', texture_entries())
            document = json.loads(bundle.read_text())
            template = document['texture_dependencies'][0]
            document['texture_dependencies'] = [dict(template, slot=index)
                                                 for index in range(module.MAX_TEXTURE_DEPENDENCIES + 1)]
            bundle.write_text(json.dumps(document))
            original = module.extract_entry
            seen = []

            def spy(archive, entry):
                seen.append(entry['name'])
                return original(archive, entry)

            module.extract_entry = spy
            try:
                with self.assertRaisesRegex(ValueError, 'more texture dependencies'):
                    prepare(bundle, root / 'game', root / 'prepared')
            finally:
                module.extract_entry = original
            self.assertEqual(seen, [])

    def test_every_successful_prepare_verifies(self):
        cases = {'default': {}, 'stored': {'archives': ['iw_00.iwd']},
                 'single-texture': {'bundle_overrides': {'textures': ('me_concrete_dest02_col',)}},
                 'repeated': {'bundle_overrides': {'textures': ('me_concrete_dest02_col',) * 3}}}
        for label, options in cases.items():
            with tempfile.TemporaryDirectory() as temporary:
                with self.subTest(case=label):
                    _, out, _, _ = run(temporary, **options)
                    self.assertEqual(verify_prepared_manifest(out)['document'],
                                     'kisak_static_world_surface_bundle')

    def test_verify_only_rejects_a_self_consistent_malformed_shader(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, out, _, _ = run(temporary)
            broken = shader_blob('vertex', body=[((2 << 24) | MOV, 0x800F0000)], end=False)
            rewrite_prepared(out, {'vertex_shader_bytecode': broken})
            with self.assertRaisesRegex(ValueError, 'overruns the bytecode'):
                verify_prepared_manifest(out)

    def test_verify_only_rejects_caps_that_do_not_follow_from_the_export(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, out, _, _ = run(temporary)
            manifest = json.loads((out / MANIFEST_NAME).read_text())
            manifest['trusted_caps']['max_shader_bytes'] = 64
            (out / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, 'trusted caps do not follow'):
                verify_prepared_manifest(out)
        with tempfile.TemporaryDirectory() as temporary:
            _, out, _, _ = run(temporary)
            rewrite_prepared(out, {}, caps={'max_bundle_bytes': 67108864, 'max_shader_bytes': 8,
                                            'max_bundle_vertices': 65535, 'max_bundle_indices': 1048576})
            with self.assertRaisesRegex(ValueError, 'exceeds the shader cap'):
                verify_prepared_manifest(out)

    def test_verify_only_rejects_missing_or_repeated_roles(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, out, _, _ = run(temporary)
            manifest = json.loads((out / MANIFEST_NAME).read_text())
            manifest['artifacts'] = manifest['artifacts'][:-1]
            (out / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, 'cover exactly the required roles'):
                verify_prepared_manifest(out)
            manifest['artifacts'] = manifest['artifacts'] + [dict(manifest['artifacts'][0])]
            (out / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, 'role is repeated'):
                verify_prepared_manifest(out)

    def test_verify_only_rejects_rewritten_shader_checks(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, out, _, _ = run(temporary)
            manifest = json.loads((out / MANIFEST_NAME).read_text())
            manifest['shader_checks']['vertex']['instruction_count'] = 99
            (out / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, 'shader checks do not follow'):
                verify_prepared_manifest(out)

    def test_verify_only_recomputes_everything_and_returns_the_export(self):
        with tempfile.TemporaryDirectory() as temporary:
            bundle, out, manifest, _ = run(temporary)
            export = verify_prepared_manifest(out)
            self.assertEqual(export, json.loads(bundle.read_text()))
            self.assertEqual(export['document'], 'kisak_static_world_surface_bundle')
            self.assertIsNone(next(item for item in export['files']
                                   if item['role'] == 'world_vertices')['sha256'])
            self.assertEqual(sha256((out / EXPORT_NAME).read_bytes()), manifest['source']['sha256'])

    def test_verify_only_detects_tampered_bytes(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, out, _, _ = run(temporary)
            target = out / 'world-surface-indices.bin'
            original = target.read_bytes()
            target.write_bytes(bytes([original[0] ^ 0xFF]) + original[1:])
            with self.assertRaisesRegex(ValueError, 'SHA-256'):
                verify_prepared_manifest(out)
            target.write_bytes(original)
            self.assertEqual(verify_prepared_manifest(out)['schema_version'], 1)

    def test_verify_only_detects_a_tampered_export_sibling_and_manifest(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, out, manifest, _ = run(temporary)
            export_path = out / EXPORT_NAME
            original = export_path.read_bytes()
            export_path.write_bytes(original.replace(b'mp_test', b'mp_other'))
            with self.assertRaisesRegex(ValueError, 'export sibling'):
                verify_prepared_manifest(out)
            export_path.write_bytes(original)
            manifest_path = out / MANIFEST_NAME
            tampered = dict(manifest)
            tampered['bundle_digest'] = dict(manifest['bundle_digest'], value='0' * 64)
            manifest_path.write_text(json.dumps(tampered, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, 'bundle digest'):
                verify_prepared_manifest(out)
            manifest_path.write_text(json.dumps(dict(manifest, render_ready=True), indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, 'claims render readiness'):
                verify_prepared_manifest(out)

    def test_verify_only_rejects_truncated_and_escaping_files(self):
        with tempfile.TemporaryDirectory() as temporary:
            _, out, manifest, _ = run(temporary)
            target = out / 'world-surface-vs.bin'
            target.write_bytes(target.read_bytes()[:-4])
            with self.assertRaisesRegex(ValueError, 'length disagrees'):
                verify_prepared_manifest(out)
        with tempfile.TemporaryDirectory() as temporary:
            _, out, _, _ = run(temporary)
            manifest_path = out / MANIFEST_NAME
            document = json.loads(manifest_path.read_text())
            document['artifacts'][0]['file'] = '../escape.bin'
            manifest_path.write_text(json.dumps(document, indent=2) + '\n')
            with self.assertRaisesRegex(ValueError, 'leaves the prepared directory'):
                verify_prepared_manifest(out)


def rewrite_prepared(root, replacements, caps=None):
    """Rewrite payload bytes together with every digest that describes them.

    This keeps the manifest and the export sibling self-consistent, so a test can
    prove verify-only rejects on structure rather than on a hash mismatch.
    """
    root = Path(root)
    manifest = json.loads((root / MANIFEST_NAME).read_text())
    export = json.loads((root / EXPORT_NAME).read_text())
    for role, data in replacements.items():
        record = next(item for item in export['files'] if item['role'] == role)
        (root / record['name']).write_bytes(data)
        record['bytes'], record['fnv1a64'] = len(data), fnv(data)
        artifact = next(item for item in manifest['artifacts'] if item['role'] == role)
        artifact['bytes'], artifact['sha256'], artifact['fnv1a64'] = len(data), sha256(data), fnv(data)
        if role in ('vertex_shader_bytecode', 'pixel_shader_bytecode'):
            stage = 'vertex' if role.startswith('vertex') else 'pixel'
            shader = export['pass']['shaders'][stage]
            shader['byte_count'], shader['word_count'] = len(data), len(data) // 4
            shader['fnv1a64'] = fnv(data)
    if caps is not None:
        export['caps'] = dict(caps)
        manifest['caps'] = dict(caps)
    export['bytes']['payload_total'] = sum(item['bytes'] for item in export['files'])
    raw = json.dumps(export).encode()
    (root / EXPORT_NAME).write_bytes(raw)
    manifest['export'] = export
    manifest['source']['sha256'], manifest['source']['bytes'] = sha256(raw), len(raw)
    manifest['bytes'] = export['bytes']
    manifest['trusted_caps'] = module.trusted_caps(export)
    manifest['bundle_digest']['value'] = module.bundle_digest(manifest['artifacts'], manifest['textures'])
    (root / MANIFEST_NAME).write_text(json.dumps(manifest, indent=2) + '\n')
    return manifest


def patch(path, offset, data):
    with open(path, 'r+b') as stream:
        stream.seek(offset)
        stream.write(data)
    return path


def announce_zip64(path):
    """Mark both end-record entry counts, which is how a ZIP64 archive announces itself."""
    end = path.stat().st_size - 22
    patch(path, end + 8, struct.pack('<H', 0xFFFF))
    return patch(path, end + 10, struct.pack('<H', 0xFFFF))


def central_offset(path):
    raw = Path(path).read_bytes()
    position = raw.rfind(b'PK\x05\x06')
    return struct.unpack_from('<I', raw, position + 16)[0]


def local_offset(path):
    raw = Path(path).read_bytes()
    position = raw.rfind(b'PK\x05\x06')
    directory = struct.unpack_from('<I', raw, position + 16)[0]
    return struct.unpack_from('<I', raw, directory + 42)[0]


if __name__ == '__main__':
    unittest.main()
