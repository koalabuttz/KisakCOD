#!/usr/bin/env python3
"""Prepare, and later re-verify, a strict private copy of one static world surface bundle.

The offline export writes one bundle document plus four payload files. This step
re-verifies that document against those bytes, copies the exact texture bytes the
material depends on out of the authorized main/*.iwd archives, and writes a new
private directory holding byte-identical copies, the export document verbatim as
`export.json`, and a manifest that adds SHA-256 provenance.

Reading is bounded by limits this file owns, never by the document it is reading.
A document may lower a limit; it can never raise one. Every size is checked from
stat against the remaining budget before a byte is read, the read itself asks for
exactly the declared length, and one extra read proves the file did not grow.

Shader checks are a framing proof, not a validator. They follow the D3D9 token
layout from d3d9types.h: opcode is bits 0..15, instruction length is bits 24..27
(D3DSI_INSTLENGTH_MASK 0x0F000000, shift 24) and counts the DWORDs after the
instruction token, comment length is bits 16..30 (D3DSI_COMMENTSIZE_MASK
0x7FFF0000, shift 16) and counts the DWORDs after the comment token, END is
0x0000FFFF and must be the last DWORD. The same rules live in the C++ worker's
ValidateShaderBytecode, so a blob that passes here frames identically there.

They say nothing about what an instruction means, what operands an opcode takes,
or what constants a shader reads. Texture bytes are copied, never decoded:
DecodeIwiMipChain owns that.

Prepared directory layout:

  world-surface-vs.bin, ...   copied payload, byte-identical to the export
  images/<name>.iwi           genuine packaged IWI, byte-identical, still encoded
  export.json                 the export document, byte-identical to the source
  world-surface-manifest.json written last, adding SHA-256 for everything above

The manifest does not hash itself; this tool prints the manifest's own SHA-256.
`export.json` is the sibling the C++ consumer reads: it keeps the export's own
document identity and its deliberate `files[].sha256: null`, while integrity lives
in the manifest's `artifacts[]` and `bundle_digest`.

`--verify-only` re-runs every check from disk with the same bounds, without
writing anything, and returns the verbatim export document.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import stat
import struct
import zlib

MANIFEST_NAME = 'world-surface-manifest.json'
EXPORT_NAME = 'export.json'
IMAGES_DIRECTORY = 'images'

# Limits this file trusts. A bundle document may ask for less, never for more.
MAX_PAYLOAD_BYTES = 64 * 1024 * 1024      # trusted aggregate of copied payload bytes
MAX_SHADER_BYTES = 256 * 1024             # trusted per-shader bytecode cap
MAX_VERTICES = 65535
MAX_INDICES = 1048576
MAX_DOCUMENT_BYTES = 8 * 1024 * 1024
MAX_ARCHIVES = 64
MAX_ARCHIVE_ENTRIES = 200000
MAX_CENTRAL_DIRECTORY_BYTES = 16 * 1024 * 1024
MAX_ENTRY_BYTES = 32 * 1024 * 1024        # matches DecodeIwiMipChain's own file bound
MAX_TEXTURE_DEPENDENCIES = 64
REQUIRED_ROLES = frozenset(('vertex_shader_bytecode', 'pixel_shader_bytecode',
                            'world_vertices', 'world_indices'))
MAX_COMPRESSION_RATIO = 256
MAX_ENTRY_NAME_BYTES = 260
MAX_LOCAL_EXTRA_BYTES = 4096
READ_CHUNK_BYTES = 1 << 20

# D3D9 shader token framing, from d3d9types.h.
D3DSI_OPCODE_MASK = 0x0000FFFF
D3DSI_INSTLENGTH_MASK = 0x0F000000
D3DSI_INSTLENGTH_SHIFT = 24
D3DSI_COMMENTSIZE_MASK = 0x7FFF0000
D3DSI_COMMENTSIZE_SHIFT = 16
D3DSIO_NOP = 0x0000
D3DSIO_COMMENT = 0x0000FFFE
D3DSIO_END = 0x0000FFFF
STAGE_VERSION_TOKEN = {'vertex': 0xFFFE0300, 'pixel': 0xFFFF0300}   # vs_3_0 / ps_3_0
SHADER_FILE_ROLE = {'vertex': 'vertex_shader_bytecode', 'pixel': 'pixel_shader_bytecode'}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def fnv1a64(data):
    digest = 0xcbf29ce484222325
    for byte in data:
        digest = ((digest ^ byte) * 0x100000001b3) % (1 << 64)
    return '%016x' % digest


def plain_name(name):
    """True for a file name that cannot address anything outside its directory."""
    return (isinstance(name, str) and 0 < len(name) <= 255 and name not in ('.', '..')
            and all(32 <= ord(c) < 127 for c in name)
            and not any(c in name for c in '/\\:'))


def entry_path(raw):
    """Normalize an archive entry name, rejecting anything that escapes the root."""
    if isinstance(raw, (bytes, bytearray)):
        require(len(raw) <= MAX_ENTRY_NAME_BYTES, 'archive entry name exceeds the bound')
        try:
            name = raw.decode('ascii')
        except UnicodeDecodeError:
            raise ValueError('archive entry name is not ASCII') from None
    elif isinstance(raw, str):
        require(len(raw) <= MAX_ENTRY_NAME_BYTES, 'archive entry name exceeds the bound')
        name = raw
    else:
        raise ValueError('archive entry name is missing')
    require(name and all(32 <= ord(c) < 127 for c in name), 'archive entry name is not printable ASCII')
    normalized = name.replace('\\', '/')
    require(not normalized.startswith('/') and ':' not in normalized, 'archive entry name is not relative')
    parts = normalized.split('/')
    require(all(part not in ('', '.', '..') for part in parts), 'archive entry path escapes the archive root')
    return '/'.join(parts).lower()


def open_readonly(path):
    """Single place that opens a file: read-only, and never through a symlink."""
    return os.open(path, os.O_RDONLY | os.O_NOFOLLOW)


def read_regular_file(path, declared_bytes, budget, label):
    """Check the budget from stat, then read exactly the declared length and no more."""
    path = Path(path)
    status = path.lstat()
    require(not stat.S_ISLNK(status.st_mode) and stat.S_ISREG(status.st_mode),
            '%s must be a regular file' % label)
    require(status.st_size <= budget, '%s exceeds the remaining read budget' % label)
    require(status.st_size == declared_bytes, '%s length disagrees with the document' % label)
    descriptor = open_readonly(path)
    try:
        chunks, remaining = [], declared_bytes
        while remaining:
            chunk = os.read(descriptor, min(remaining, READ_CHUNK_BYTES))
            require(chunk, '%s shrank while it was read' % label)
            chunks.append(chunk)
            remaining -= len(chunk)
        require(not os.read(descriptor, 1), '%s grew while it was read' % label)
    finally:
        os.close(descriptor)
    return b''.join(chunks)


def read_document(path):
    path = Path(path)
    status = path.lstat()
    require(not stat.S_ISLNK(status.st_mode) and stat.S_ISREG(status.st_mode),
            'the bundle document must be a regular file')
    require(status.st_size <= MAX_DOCUMENT_BYTES, 'the bundle document exceeds its bound')
    raw = read_regular_file(path, status.st_size, MAX_DOCUMENT_BYTES, 'the bundle document')
    try:
        document = json.loads(raw.decode('utf-8'))
    except (UnicodeDecodeError, json.JSONDecodeError):
        raise ValueError('the document is not UTF-8 JSON') from None
    require(isinstance(document, dict), 'the document must be a JSON object')
    return raw, document


def read_bundle(path):
    raw, document = read_document(path)
    require(document.get('schema_version') == 1, 'unsupported bundle schema version')
    require(document.get('document') == 'kisak_static_world_surface_bundle', 'unexpected bundle document')
    require(document.get('render_ready') is False, 'the bundle already claims render readiness')
    require(document.get('replay_claimed') is False, 'the bundle already claims replay')
    return raw, document


def trusted_caps(document):
    """Clamp the document's caps to the limits this file owns."""
    declared = document.get('caps')
    require(isinstance(declared, dict), 'the bundle must declare its caps')
    caps = {}
    for name, trusted in (('max_bundle_bytes', MAX_PAYLOAD_BYTES), ('max_shader_bytes', MAX_SHADER_BYTES),
                          ('max_bundle_vertices', MAX_VERTICES), ('max_bundle_indices', MAX_INDICES)):
        value = declared.get(name)
        require(isinstance(value, int) and value > 0, 'the bundle cap %s is missing' % name)
        caps[name] = min(value, trusted)
    return caps


def read_payload(document, source, caps):
    """Read every declared payload file, checking each budget before any read."""
    files = document.get('files')
    require(isinstance(files, list) and files, 'the bundle must declare its payload files')
    payloads, total = {}, 0
    for entry in files:
        require(isinstance(entry, dict), 'a payload file entry must be an object')
        role, name, size = entry.get('role'), entry.get('name'), entry.get('bytes')
        require(plain_name(name), 'a payload file name is not a plain file name')
        require(isinstance(role, str) and role and role not in payloads, 'a payload role is missing or repeated')
        require(isinstance(size, int) and size >= 0, 'a payload file length is not a byte count')
        if role in SHADER_FILE_ROLE.values():
            require(size <= caps['max_shader_bytes'], 'payload file %s exceeds the shader cap' % name)
        payloads[role] = read_regular_file(source / name, size, caps['max_bundle_bytes'] - total,
                                           'payload file %s' % name)
        total += len(payloads[role])
    declared = document.get('bytes')
    require(isinstance(declared, dict), 'the bundle must declare its payload bytes')
    require(declared.get('payload_total') == total, 'the declared payload total disagrees with the payload files')
    return payloads


def derive_artifacts(document, payloads):
    """Rebuild the artifact records from verified bytes; the export must agree exactly."""
    artifacts, names = [], set()
    for entry in document.get('files') or []:
        require(isinstance(entry, dict), 'a payload file entry must be an object')
        role, name, size = entry.get('role'), entry.get('name'), entry.get('bytes')
        require(plain_name(name), 'a payload file name is not a plain file name')
        require(role in payloads, 'the export names a role without a payload')
        require(name not in names, 'a payload file name is repeated')
        names.add(name)
        data = payloads[role]
        require(len(data) == size, 'payload file %s length disagrees with the document' % name)
        require(fnv1a64(data) == entry.get('fnv1a64'),
                'payload file %s does not carry the digest the bundle declares' % name)
        digest = sha256(data)
        require(entry.get('sha256') in (None, digest), 'payload file %s disagrees with its declared SHA-256' % name)
        artifacts.append({'role': role, 'file': name, 'bytes': len(data),
                          'sha256': digest, 'fnv1a64': entry['fnv1a64'], 'verified': True})
    return artifacts


def check_artifact_roles(artifacts):
    """The copied set must be exactly the four roles, once each, under distinct names."""
    require(isinstance(artifacts, list) and artifacts, 'no artifacts are declared')
    roles = [item.get('role') for item in artifacts]
    require(all(isinstance(role, str) and role for role in roles), 'an artifact role is missing')
    require(len(set(roles)) == len(roles), 'an artifact role is repeated')
    require(set(roles) == REQUIRED_ROLES, 'the artifacts do not cover exactly the required roles')
    names = [item.get('file') for item in artifacts]
    require(len(set(names)) == len(names), 'an artifact file name is repeated')


def check_declared_shaders(document, artifacts, caps):
    """The export's own shader records must agree with the payload it shipped."""
    by_role = {artifact['role']: artifact for artifact in artifacts}
    shaders = (document.get('pass') or {}).get('shaders')
    require(isinstance(shaders, dict), 'the bundle must describe its pass shaders')
    for role, key in SHADER_FILE_ROLE.items():
        shader = shaders.get(role)
        require(isinstance(shader, dict), 'the bundle must describe its %s shader' % role)
        require(key in by_role, 'the bundle does not ship a %s payload' % role)
        require(shader.get('file') == by_role[key]['file'], 'the %s shader names another payload file' % role)
        require(shader.get('byte_count') == by_role[key]['bytes'], 'the %s shader byte count disagrees' % role)
        require(shader.get('word_count') is not None and shader['word_count'] * 4 == shader['byte_count'],
                'the %s shader word count does not fill its bytes' % role)
        require(shader['byte_count'] <= caps['max_shader_bytes'], 'the %s shader exceeds the shader cap' % role)
        require(shader.get('fnv1a64') == by_role[key]['fnv1a64'], 'the %s shader digest disagrees' % role)


def check_geometry(document, artifacts, caps):
    """The slice must account for every payload byte, and never past the caps."""
    geometry, slice_block = document.get('geometry'), document.get('slice')
    require(isinstance(geometry, dict) and isinstance(slice_block, dict),
            'the bundle must describe its geometry and its slice')
    by_role = {artifact['role']: artifact for artifact in artifacts}
    require('world_vertices' in by_role and 'world_indices' in by_role,
            'the bundle must ship vertex and index payloads')
    vertices, indices = by_role['world_vertices'], by_role['world_indices']
    require(slice_block.get('vertex_file') == vertices['file'], 'the vertex slice names another payload file')
    require(slice_block.get('index_file') == indices['file'], 'the index slice names another payload file')
    require(slice_block.get('vertex_bytes') == vertices['bytes'], 'the vertex slice length disagrees')
    require(slice_block.get('index_bytes') == indices['bytes'], 'the index slice length disagrees')
    require(geometry.get('vertex_stride') == slice_block.get('vertex_stride'),
            'the vertex stride disagrees between geometry and slice')
    require(geometry.get('vertex_count', 0) * geometry['vertex_stride'] == vertices['bytes'],
            'the vertex count and stride do not fill the vertex payload')
    require(geometry.get('index_count', 0) * slice_block.get('index_stride', 0) == indices['bytes'],
            'the index count and stride do not fill the index payload')
    require(geometry['vertex_count'] <= caps['max_bundle_vertices'], 'the vertex count exceeds its cap')
    require(geometry['index_count'] <= caps['max_bundle_indices'], 'the index count exceeds its cap')
    request, surface = document.get('request') or {}, document.get('surface') or {}
    require(request.get('surface_index') == surface.get('index'),
            'the request and the surface describe different surfaces')


class Archive:
    """Read-only, bounds-checked view of one ZIP archive."""

    def __init__(self, path):
        self.path = Path(path)
        self.fd = open_readonly(self.path)
        self.size = os.fstat(self.fd).st_size
        try:
            self.entries = self._index()
        except BaseException:
            os.close(self.fd)
            raise

    def close(self):
        os.close(self.fd)

    def read(self, offset, length):
        require(0 <= offset and 0 <= length and offset + length <= self.size,
                'an archive read falls outside the file')
        data = os.pread(self.fd, length, offset)
        require(len(data) == length, 'an archive read was short')
        return data

    def _index(self):
        tail_length = min(self.size, 22 + 65535)
        require(tail_length >= 22, 'archive is smaller than an end-of-central-directory record')
        tail = self.read(self.size - tail_length, tail_length)
        position = tail.rfind(b'PK\x05\x06')
        require(position >= 0, 'archive %s has no end-of-central-directory record' % self.path.name)
        require(position + 22 <= len(tail), 'archive %s has a truncated end record' % self.path.name)
        (_, disk, directory_disk, entries_here, entries_total, directory_size, directory_offset,
         comment_length) = struct.unpack_from('<IHHHHIIH', tail, position)
        require(position + 22 + comment_length <= len(tail), 'archive %s has a truncated comment' % self.path.name)
        require(disk == 0 and directory_disk == 0 and entries_here == entries_total,
                'archive %s is a multi-disk archive' % self.path.name)
        require(entries_total != 0xFFFF and directory_size != 0xFFFFFFFF and directory_offset != 0xFFFFFFFF,
                'archive %s needs the ZIP64 format, which is unsupported' % self.path.name)
        require(entries_total <= MAX_ARCHIVE_ENTRIES, 'archive %s exceeds the entry cap' % self.path.name)
        require(directory_size <= MAX_CENTRAL_DIRECTORY_BYTES,
                'archive %s exceeds the central directory cap' % self.path.name)
        require(directory_offset + directory_size <= self.size,
                'archive %s has a central directory outside the file' % self.path.name)
        directory = self.read(directory_offset, directory_size)
        entries, position = {}, 0
        for _ in range(entries_total):
            require(position + 46 <= len(directory), 'archive %s has a truncated central directory' % self.path.name)
            (signature, _, _, _, method, _, _, crc, compressed, uncompressed, name_length, extra_length,
             entry_comment, _, _, _, local) = struct.unpack_from('<IHHHHHHIIIHHHHHII', directory, position)
            require(signature == 0x02014B50, 'archive %s has a bad central directory signature' % self.path.name)
            require(position + 46 + name_length + extra_length + entry_comment <= len(directory),
                    'archive %s has a truncated central directory entry' % self.path.name)
            raw_name = directory[position + 46:position + 46 + name_length]
            if raw_name.endswith((b'/', b'\\')):
                entry_path(raw_name[:-1])
                position += 46 + name_length + extra_length + entry_comment
                continue
            name = entry_path(raw_name)
            require(local + 30 <= self.size, 'archive %s has a local header outside the file' % self.path.name)
            entries.setdefault(name, []).append({'name': name, 'method': method, 'crc': crc,
                                                 'compressed': compressed, 'uncompressed': uncompressed,
                                                 'local': local, 'archive': self.path.name})
            position += 46 + name_length + extra_length + entry_comment
        return entries


def archive_directory(game_dir):
    game = Path(game_dir).expanduser()
    main = game / 'main'
    return main if main.is_dir() else game


def open_archives(directory):
    directory = Path(directory)
    require(directory.is_dir(), 'the game install has no main directory')
    paths = sorted(directory.glob('*.iwd'), key=lambda path: path.name)
    require(paths, 'the game install has no main archives')
    require(len(paths) <= MAX_ARCHIVES, 'the archive count exceeds the bound')
    archives = []
    try:
        for path in paths:
            require(stat.S_ISREG(path.lstat().st_mode), 'archive %s is not a regular file' % path.name)
            archives.append(Archive(path))
    except BaseException:
        for archive in archives:
            archive.close()
        raise
    return archives


def extract_entry(archive, entry):
    """Read one entry with every read bounded by the caps, then check CRC-32."""
    require(entry['method'] in (0, 8), 'archive entry %s uses an unsupported compression method' % entry['name'])
    require(entry['uncompressed'] <= MAX_ENTRY_BYTES, 'archive entry %s declares more bytes than allowed' % entry['name'])
    require(entry['compressed'] <= MAX_ENTRY_BYTES, 'archive entry %s stores more bytes than allowed' % entry['name'])
    require(entry['uncompressed'] <= MAX_COMPRESSION_RATIO * max(entry['compressed'], 1),
            'archive entry %s exceeds the compression ratio bound' % entry['name'])
    header = archive.read(entry['local'], 30)
    require(struct.unpack_from('<I', header, 0)[0] == 0x04034B50,
            'archive entry %s has a bad local header signature' % entry['name'])
    name_length, extra_length = struct.unpack_from('<HH', header, 26)
    require(name_length <= MAX_ENTRY_NAME_BYTES and extra_length <= MAX_LOCAL_EXTRA_BYTES,
            'archive entry %s has an oversized local header' % entry['name'])
    start = entry['local'] + 30 + name_length + extra_length
    require(start + entry['compressed'] <= archive.size, 'archive entry %s has data outside the file' % entry['name'])
    raw = archive.read(start, entry['compressed'])
    if entry['method'] == 0:
        require(entry['compressed'] == entry['uncompressed'],
                'stored archive entry %s declares two different lengths' % entry['name'])
        data = raw
    else:
        stream = zlib.decompressobj(-15)
        data = stream.decompress(raw, entry['uncompressed'] + 1)
        require(stream.eof and not stream.unconsumed_tail and not stream.unused_data,
                'archive entry %s does not decompress to exactly its declared size' % entry['name'])
    require(len(data) == entry['uncompressed'], 'archive entry %s length disagrees with its directory' % entry['name'])
    require(zlib.crc32(data) % (1 << 32) == entry['crc'], 'archive entry %s fails its CRC-32' % entry['name'])
    return data


def locate_entry(archives, name):
    """Find every copy of an entry in the authorized archives, reading no payload yet."""
    candidates = [(archive, entry) for archive in archives for entry in archive.entries.get(name, ())]
    require(candidates, 'no authorized archive provides %s' % name)
    return candidates


def extract_candidates(candidates):
    """Extract every copy; keep the bytes only when all copies are identical."""
    resolved = [(archive, entry, extract_entry(archive, entry)) for archive, entry in candidates]
    data = resolved[0][2]
    digest = sha256(data)
    for _, _, other in resolved[1:]:
        require(len(other) == len(data) and sha256(other) == digest,
                'authorized archives disagree about the bytes of %s' % resolved[0][1]['name'])
    entry = resolved[0][1]
    provenance = {'archive': entry['archive'], 'entry': entry['name'], 'method': entry['method'],
                  'compressed_bytes': entry['compressed'], 'bytes': entry['uncompressed'],
                  'crc32': entry['crc'], 'sha256': digest,
                  'copies': [{'archive': item[1]['archive'], 'compressed_bytes': item[1]['compressed']}
                             for item in resolved]}
    return data, provenance


def check_shader(data, role, caps):
    """Prove the token framing of one shader blob; never its meaning.

    Framing contract, identical to the C++ worker's ValidateShaderBytecode:
      * whole DWORDs, non-empty, within the shader cap;
      * the first DWORD is exactly the stage's 3.0 version token;
      * COMMENT carries its length in bits 16..30 and skips 1 + length DWORDs;
      * any other opcode with a non-zero length in bits 24..27 skips 1 + length;
      * a zero length is only legal for NOP, so an unsupported shape fails closed;
      * END must exist and must be the last DWORD, so an operand that happens to
        look like END cannot terminate the walk.
    """
    require(len(data) % 4 == 0, 'the %s shader bytecode is not a whole number of words' % role)
    require(len(data) <= caps['max_shader_bytes'], 'the %s shader exceeds the shader cap' % role)
    words = struct.unpack('<%dI' % (len(data) // 4), data)
    require(len(words) >= 2, 'the %s shader is shorter than a version token and an end token' % role)
    expected = STAGE_VERSION_TOKEN[role]
    require(words[0] == expected,
            'the %s shader version token is 0x%08x, not the supported 0x%08x' % (role, words[0], expected))
    offset, instructions, comments = 1, 0, 0
    while True:
        require(offset < len(words), 'the %s shader ends without an end token' % role)
        token = words[offset]
        opcode = token & D3DSI_OPCODE_MASK
        if opcode == D3DSIO_END:
            require(offset + 1 == len(words),
                    'the %s shader has an end token at DWORD %d of %d, so DWORDs follow it'
                    % (role, offset, len(words)))
            break
        if opcode == D3DSIO_COMMENT:
            length = (token & D3DSI_COMMENTSIZE_MASK) >> D3DSI_COMMENTSIZE_SHIFT
            require(length != 0, 'the %s shader has a comment token declaring zero length' % role)
            require(offset + 1 + length <= len(words),
                    'a %s shader comment at DWORD %d declares %d DWORDs and overruns the bytecode'
                    % (role, offset, length))
            comments += 1
            offset += 1 + length
            continue
        length = (token & D3DSI_INSTLENGTH_MASK) >> D3DSI_INSTLENGTH_SHIFT
        if length:
            require(offset + 1 + length <= len(words),
                    'the %s shader instruction at DWORD %d declares length %d and overruns the bytecode'
                    % (role, offset, length))
        else:
            require(opcode == D3DSIO_NOP,
                    'the %s shader opcode 0x%04x at DWORD %d declares zero length, which is unsupported'
                    % (role, opcode, offset))
        instructions += 1
        offset += 1 + length
    return {'role': role, 'stage': role, 'version': '0x%08x' % words[0],
            'major': (words[0] >> 8) % 256, 'minor': words[0] % 256,
            'words': len(words), 'instruction_count': instructions, 'comment_count': comments,
            'end_offset': offset, 'trailing_words': 0,
            'structural_status': 'validated', 'semantics_validated': False,
            'framing_contract': 'd3d9 token framing: opcode bits 0..15, instruction length bits 24..27, '
                                'comment length bits 16..30, END must be the last DWORD',
            'semantics_note': 'framing only: no instruction, operand, constant or state meaning is checked'}


def check_pins(textures, pins):
    by_entry = {texture['iwi_entry']: texture for texture in textures}
    for name, (digest, size) in pins.items():
        require(name in by_entry, 'the bundle does not depend on the pinned texture %s' % name)
        data = by_entry[name]['data']
        require(len(data) == size and sha256(data) == digest,
                'the extracted bytes of %s are not the pinned bytes' % name)


def parse_pins(values):
    pins = {}
    for value in values:
        name, _, spec = value.partition('=')
        digest, _, size = spec.partition(':')
        require(len(digest) == 64 and all(c in '0123456789abcdefABCDEF' for c in digest) and size.isdigit(),
                'a pinned texture must be given as ENTRY=SHA256:BYTES with a 64-digit digest')
        pins[entry_path(name)] = (digest.lower(), int(size))
    return pins


def reject_symlink_ancestors(path):
    probe = Path(path)
    while True:
        if probe.is_symlink():
            raise ValueError('the output path crosses a symbolic link')
        if probe.parent == probe:
            return
        probe = probe.parent


def private_directory(path):
    """Create a brand new 0700 directory, refusing to reuse or follow anything."""
    reject_symlink_ancestors(Path(path).parent)
    require(not Path(path).exists() and not Path(path).is_symlink(), 'the output directory must not exist yet')
    os.mkdir(path, 0o700)
    os.chmod(path, 0o700)
    status = os.stat(path, follow_symlinks=False)
    require(stat.S_ISDIR(status.st_mode) and status.st_uid == os.getuid()
            and stat.S_IMODE(status.st_mode) == 0o700, 'the output directory is not private')
    return Path(path)


def write_new(directory, name, data, created):
    """Write one file that must not exist, without following a symlink."""
    require(plain_name(name), 'a prepared file name is not a plain file name')
    target = Path(directory) / name
    require(not target.is_symlink() and not target.exists(), 'refusing to overwrite %s' % name)
    descriptor = os.open(target, os.O_WRONLY | os.O_CREAT | os.O_EXCL | os.O_NOFOLLOW, 0o600)
    created.append(target)
    try:
        os.fchmod(descriptor, 0o600)
        view = memoryview(data)
        while view:
            view = view[os.write(descriptor, view):]
        os.fsync(descriptor)
    finally:
        os.close(descriptor)
    return target


def prepared_path(root, relative):
    """Resolve a prepared file name, allowing only the images/ subdirectory."""
    require(isinstance(relative, str), 'a prepared file name is missing')
    parts = relative.split('/')
    require(len(parts) == 1 or (len(parts) == 2 and parts[0] == IMAGES_DIRECTORY),
            'a prepared file name leaves the prepared directory')
    require(plain_name(parts[-1]), 'a prepared file name is not a plain file name')
    return Path(root).joinpath(*parts)


def bundle_digest(artifacts, textures):
    lines = ['%s\t%s\t%d\t%s\n' % (item['role'], item['file'], item['bytes'], item['sha256'])
             for item in artifacts]
    lines += ['texture\t%s\t%d\t%s\n' % (item['iwi_entry'], item['bytes'], item['sha256'])
              for item in textures]
    return sha256(''.join(sorted(lines)).encode('ascii'))


def build_manifest(document, bundle_path, bundle_raw, artifacts, textures, shader_checks, caps):
    unresolved = document.get('unresolved') or []
    entries = [texture['iwi_entry'] for texture in textures]
    resolved = []
    for reason in ('texture_payload_not_captured', 'material_texture_payload_not_captured'):
        if entries and reason in [item.get('reason') for item in unresolved]:
            resolved.append({'reason': reason, 'by': 'iwi_extraction', 'entries': entries})
    texture_records = [{'slot': texture['dependency'].get('slot'),
                        'semantic': texture['dependency'].get('semantic'),
                        'iwi_entry': texture['iwi_entry'],
                        'file': texture['file'],
                        'bytes': len(texture['data']),
                        'sha256': sha256(texture['data']),
                        'decoded': False,
                        'provenance': texture['provenance'],
                        'dependency': texture['dependency']} for texture in textures]
    return {
        'schema_version': 1,
        'document': 'kisak_world_surface_prepared_bundle',
        'tool': 'strict offline world surface preparation',
        'render_ready': False,
        'replay_claimed': False,
        'shader_semantics_validated': False,
        'source': {'bundle': str(bundle_path), 'bytes': len(bundle_raw), 'sha256': sha256(bundle_raw),
                   'schema_version': document['schema_version'], 'document': document['document'],
                   'render_ready': document['render_ready'], 'replay_claimed': document['replay_claimed'],
                   'export_file': EXPORT_NAME},
        'request': document.get('request'),
        'world': document.get('world'),
        'surface': document.get('surface'),
        'geometry': document.get('geometry'),
        'slice': document.get('slice'),
        'material': document.get('material'),
        'technique': document.get('technique'),
        'declaration': document.get('declaration'),
        'pass': document.get('pass'),
        'caps': document.get('caps'),
        'trusted_caps': caps,
        'bytes': document.get('bytes'),
        'artifacts': artifacts,
        'textures': texture_records,
        'shader_checks': shader_checks,
        'replay': {'structural_eligible': True,
                   'reject_before_create': False,
                   'reasons': [],
                   'context_unresolved': [item.get('reason') for item in unresolved],
                   'format': None,
                   'format_note': 'no binary replay layout is defined; agree one before a consumer writes any',
                   'note': 'framing passed, which is not a rendering claim: the scene state the material '
                           'constants and code samplers need is still missing'},
        'unresolved': unresolved,
        'resolved': resolved,
        'bundle_digest': {'algorithm': 'sha256',
                          'scope': 'artifacts and textures ordered by role then entry',
                          'value': bundle_digest(artifacts, texture_records)},
        'notes': ['copied bytes are byte-identical to their sources; nothing here decodes pixels or shaders',
                  'texture bytes are the genuine packaged IWI files; DecodeIwiMipChain owns their decode',
                  'shader checks prove token framing only, never what an instruction does',
                  'reading is bounded by this tool\'s own limits; a document may lower them, never raise them',
                  'export.json is the export document verbatim and is what the C++ consumer reads',
                  'the manifest is written last and does not hash itself; the caller records its SHA-256'],
        'export': document,
    }


def verify_prepared_manifest(root):
    """Re-check a prepared directory from disk with the same bounds prepare uses.

    Hash agreement alone would let a self-consistent but malformed shader through,
    so this re-derives everything prepare derived: the trusted caps, the declared
    shader records, the geometry, the artifact roles and the shader framing, all
    from the bytes it just verified. It writes nothing and returns the export.
    """
    root = Path(root)
    require(root.is_dir(), 'the prepared directory is missing')
    raw, manifest = read_document(root / MANIFEST_NAME)
    require(manifest.get('schema_version') == 1, 'unsupported prepared manifest version')
    require(manifest.get('document') == 'kisak_world_surface_prepared_bundle',
            'the manifest is not a prepared world surface manifest')
    require(manifest.get('render_ready') is False and manifest.get('replay_claimed') is False,
            'the manifest claims render readiness or replay')
    require(manifest.get('shader_semantics_validated') is False, 'the manifest claims validated shader semantics')
    source = manifest.get('source') or {}
    require(source.get('export_file') == EXPORT_NAME, 'the manifest does not name the export sibling this tool writes')
    export_raw, export = read_document(prepared_path(root, source['export_file']))
    require(sha256(export_raw) == source.get('sha256'), 'the export sibling does not match the manifest source digest')
    require(export.get('document') == 'kisak_static_world_surface_bundle', 'the export sibling is not a bundle')
    require(export.get('schema_version') == 1, 'the export sibling has an unsupported schema version')
    require(export.get('render_ready') is False and export.get('replay_claimed') is False,
            'the export sibling claims render readiness or replay')
    caps = trusted_caps(export)
    require(manifest.get('trusted_caps') == caps, 'the manifest trusted caps do not follow from the export')
    claimed = manifest.get('artifacts')
    check_artifact_roles(claimed)
    payloads, total = {}, 0
    for artifact in claimed:
        require(isinstance(artifact, dict) and artifact.get('verified') is True, 'an artifact is not marked verified')
        role, size, digest = artifact.get('role'), artifact.get('bytes'), artifact.get('sha256')
        require(role not in payloads, 'an artifact role is repeated')
        require(isinstance(size, int) and isinstance(digest, str) and len(digest) == 64,
                'an artifact is missing its length or SHA-256')
        data = read_regular_file(prepared_path(root, artifact.get('file')), size,
                                 MAX_PAYLOAD_BYTES - total, 'prepared file %s' % artifact.get('file'))
        total += len(data)
        require(sha256(data) == digest, 'prepared file %s does not match its SHA-256' % artifact.get('file'))
        require(fnv1a64(data) == artifact.get('fnv1a64'),
                'prepared file %s does not match its FNV-1a-64' % artifact.get('file'))
        payloads[role] = data
    derived = derive_artifacts(export, payloads)
    require(derived == claimed, 'the prepared artifacts do not match the export they claim to copy')
    check_declared_shaders(export, derived, caps)
    check_geometry(export, derived, caps)
    shader_checks = {role: check_shader(payloads[SHADER_FILE_ROLE[role]], role, caps)
                     for role in ('vertex', 'pixel')}
    require(manifest.get('shader_checks') == shader_checks,
            'the manifest shader checks do not follow from the verified bytes')
    textures = manifest.get('textures')
    dependencies = export.get('texture_dependencies') or []
    require(isinstance(textures, list) and textures, 'the manifest lists no textures')
    require(len(dependencies) <= MAX_TEXTURE_DEPENDENCIES,
            'the export declares more texture dependencies than the bound allows')
    require(len(textures) == len(dependencies), 'the manifest textures do not match the export dependencies')
    records, counted = [], set()
    for texture, dependency in zip(textures, dependencies):
        require(isinstance(texture, dict) and texture.get('decoded') is False, 'a texture claims to be decoded')
        name = entry_path(dependency.get('iwi_entry'))
        require(texture.get('iwi_entry') == name, 'a texture does not match the export dependency order')
        require(texture.get('file') == IMAGES_DIRECTORY + '/' + name.rsplit('/', 1)[-1],
                'a texture file does not match its archive entry')
        size, digest = texture.get('bytes'), texture.get('sha256')
        require(isinstance(size, int) and size <= MAX_ENTRY_BYTES, 'a texture length is outside the bound')
        require(isinstance(digest, str) and len(digest) == 64, 'a texture is missing its SHA-256')
        data = read_regular_file(prepared_path(root, texture.get('file')), size,
                                 MAX_PAYLOAD_BYTES - total, 'prepared texture %s' % texture.get('file'))
        if texture['file'] not in counted:
            counted.add(texture['file'])
            total += len(data)
        require(sha256(data) == digest, 'prepared texture %s does not match its SHA-256' % texture.get('file'))
        records.append({'iwi_entry': name, 'file': texture['file'], 'bytes': len(data),
                        'sha256': digest, 'slot': dependency.get('slot'),
                        'semantic': dependency.get('semantic')})
    require(total <= MAX_PAYLOAD_BYTES, 'the prepared directory exceeds the prepared budget')
    require(manifest.get('bundle_digest', {}).get('value') == bundle_digest(derived, records),
            'the manifest bundle digest does not match its own artifacts')
    return export


def prepare(bundle_path, game_dir, out_dir, pins=None):
    bundle_path = Path(bundle_path).expanduser()
    bundle_raw, document = read_bundle(bundle_path)
    caps = trusted_caps(document)
    payloads = read_payload(document, bundle_path.parent, caps)
    artifacts = derive_artifacts(document, payloads)
    check_artifact_roles(artifacts)
    check_declared_shaders(document, artifacts, caps)
    check_geometry(document, artifacts, caps)
    shader_checks = {role: check_shader(payloads[SHADER_FILE_ROLE[role]], role, caps)
                     for role in ('vertex', 'pixel')}
    dependencies = document.get('texture_dependencies')
    require(isinstance(dependencies, list) and dependencies, 'the bundle must declare its texture dependencies')
    require(len(dependencies) <= MAX_TEXTURE_DEPENDENCIES,
            'the bundle declares more texture dependencies than the bound allows')
    out_dir = Path(out_dir).expanduser()
    game_root = Path(game_dir).expanduser().resolve()
    try:
        out_dir.resolve().relative_to(game_root)
    except ValueError:
        pass
    else:
        raise ValueError('the output directory must not live inside the game tree')
    archives = open_archives(archive_directory(game_dir))
    try:
        extracted, textures = {}, []
        budget_used = sum(artifact['bytes'] for artifact in artifacts)
        for dependency in dependencies:
            require(isinstance(dependency, dict), 'a texture dependency must be an object')
            name = entry_path(dependency.get('iwi_entry'))
            require(name.endswith('.iwi'), 'texture dependency %s is not an IWI entry' % name)
            if name not in extracted:
                candidates = locate_entry(archives, name)
                declared_bytes = candidates[0][1]['uncompressed']
                for _, other in candidates[1:]:
                    require(other['uncompressed'] == declared_bytes,
                            'authorized archives disagree about the size of %s' % name)
                require(budget_used + declared_bytes <= MAX_PAYLOAD_BYTES,
                        'texture %s does not fit the remaining prepared budget' % name)
                data, provenance = extract_candidates(candidates)
                budget_used += len(data)
                extracted[name] = (data, provenance)
            data, provenance = extracted[name]
            textures.append({'dependency': dependency, 'iwi_entry': name, 'data': data,
                             'file': IMAGES_DIRECTORY + '/' + name.rsplit('/', 1)[-1], 'provenance': provenance})
    finally:
        for archive in archives:
            archive.close()
    check_pins(textures, pins or {})
    manifest = build_manifest(document, bundle_path, bundle_raw, artifacts, textures, shader_checks, caps)
    manifest_bytes = (json.dumps(manifest, indent=2) + '\n').encode('utf-8')
    created = []
    directory = private_directory(out_dir)
    try:
        for artifact in artifacts:
            write_new(directory, artifact['file'], payloads[artifact['role']], created)
        images = private_directory(directory / IMAGES_DIRECTORY)
        written = set()
        for texture in textures:
            name = texture['file'].rsplit('/', 1)[-1]
            if name in written:
                continue
            written.add(name)
            write_new(images, name, texture['data'], created)
        write_new(directory, EXPORT_NAME, bundle_raw, created)
        write_new(directory, MANIFEST_NAME, manifest_bytes, created)
    except BaseException:
        for path in reversed(created):
            try:
                path.unlink()
            except FileNotFoundError:
                pass
        for path in (directory / IMAGES_DIRECTORY, directory):
            try:
                Path(path).rmdir()
            except OSError:
                pass
        raise
    return manifest, sha256(manifest_bytes)


def main():
    parser = argparse.ArgumentParser(description='prepare or re-verify one static world surface bundle')
    parser.add_argument('--bundle', type=Path, help='world-surface-bundle.json')
    parser.add_argument('--game', type=Path, help='game install, or its main directory')
    parser.add_argument('--out', type=Path, help='new directory to create')
    parser.add_argument('--verify-only', type=Path, metavar='ROOT',
                        help='re-verify an existing prepared directory; writes nothing')
    parser.add_argument('--require-iwi', action='append', default=[], metavar='ENTRY=SHA256:BYTES',
                        help='require exact extracted bytes for one texture dependency')
    arguments = parser.parse_args()
    if arguments.verify_only is not None:
        export = verify_prepared_manifest(arguments.verify_only)
        raw, manifest = read_document(Path(arguments.verify_only) / MANIFEST_NAME)
        print('verified', arguments.verify_only, 'manifest', sha256(raw),
              'export', manifest['source']['sha256'],
              'export_document', export.get('document'), export.get('schema_version'))
        print('artifacts', [artifact['file'] for artifact in manifest['artifacts']],
              'textures', [texture['iwi_entry'] for texture in manifest['textures']])
        return
    if not arguments.bundle or not arguments.game or not arguments.out:
        parser.error('--bundle, --game and --out are required unless --verify-only is given')
    manifest, digest = prepare(arguments.bundle, arguments.game, arguments.out,
                               parse_pins(arguments.require_iwi))
    print('prepared', arguments.out, 'manifest', MANIFEST_NAME, digest)
    print('artifacts', [artifact['file'] for artifact in manifest['artifacts']],
          'textures', [texture['iwi_entry'] for texture in manifest['textures']],
          'export', manifest['source']['export_file'])


if __name__ == '__main__':
    main()
