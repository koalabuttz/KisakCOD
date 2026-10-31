"""Encode bounded local material metadata; no game bytes are shipped here.

The native resolver validates supported shader/state identities. This serializer
preserves the exported description and resolves the image by shader name-hash.
"""
import struct


def encode_material(material):
    def word(value):
        if type(value) is not int or not 0 <= value <= 0xffffffff:
            raise ValueError("invalid unsigned material metadata value")
        return struct.pack('<I', value)

    def name(value):
        if not isinstance(value, str):
            raise ValueError("missing material metadata name")
        data = value.encode('ascii')
        if not data or len(data) > 255 or any(c < 32 or c >= 127 for c in data):
            raise ValueError("unsupported material metadata name")
        return word(len(data)) + data

    def identity(value):
        if isinstance(value, dict):
            return {k: identity(v) for k, v in value.items() if k != 'file'}
        if isinstance(value, list):
            return [identity(v) for v in value]
        return value

    technique = material['technique']
    if not technique or not technique['unlit']:
        raise ValueError('material has no unlit technique')
    unlit = technique['unlit']
    if len(unlit['passes']) != 1:
        raise ValueError('only single-pass metadata can be prepared')
    draw_pass = unlit['passes'][0]
    samplers = [a for a in draw_pass['arguments'] if a['type'] == 2]
    if len(samplers) != 1:
        raise ValueError('unlit technique must identify one material pixel sampler')
    textures = [t for t in material['textures'] if t['name_hash'] == samplers[0]['value']]
    if len(textures) != 1:
        raise ValueError('pixel sampler must resolve exactly one named image binding')
    texture = textures[0]
    state_index = material['state_bits_entry_unlit']
    if not 0 <= state_index < len(material['state_bits']):
        raise ValueError('unlit state index outside material table')
    state = material['state_bits'][state_index]
    if len(state) != 2:
        raise ValueError('invalid state bit layout')
    vertex, pixel = draw_pass['vertex_shader'], draw_pass['pixel_shader']
    if not vertex or not pixel or not vertex['bytecode_available'] or not pixel['bytecode_available']:
        raise ValueError('shader program identity unavailable')
    result = b'KUMAT01\0'
    for value in (material['name'], texture['image'], technique['set_name'], unlit['name'], vertex['name'], pixel['name']):
        result += name(value)
    values = [material['state_flags'], material['camera_region'], material['game_flags'], material['sort_key'],
              len(material['constants']), len(material['textures']), texture['semantic'], texture['sampler_state'], texture['name_hash'],
              len(unlit['passes']), unlit['flags'], draw_pass['custom_sampler_flags'], draw_pass['per_prim_count'], draw_pass['per_obj_count'], draw_pass['stable_count'],
              state_index, len(material['state_bits']), *state, vertex['word_count'], pixel['word_count'],
              int(identity(technique) == identity(material['remapped_technique']))]
    result += b''.join(word(value) for value in values)
    for shader in (vertex, pixel):
        fingerprint = shader['fnv1a64']
        if len(fingerprint) != 16:
            raise ValueError('invalid shader fingerprint')
        result += struct.pack('<Q', int(fingerprint, 16))
    args = draw_pass['arguments']
    if len(args) > 32:
        raise ValueError('shader argument count outside metadata bound')
    result += word(len(args))
    for arg in args:
        if arg['type'] in (3, 5):
            values = [arg['type'], arg['dest'], arg['code_index'], arg['first_row'], arg['row_count']]
        elif arg['type'] in (0, 2, 4, 6):
            values = [arg['type'], arg['dest'], arg['value'], 0, 0]
        else:
            raise ValueError('literal or unknown shader argument is unsupported')
        result += b''.join(word(value) for value in values)
    if len(result) > 8192:
        raise ValueError('material metadata exceeds file bound')
    return result
