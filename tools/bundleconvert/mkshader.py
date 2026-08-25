"""Builds Unity SerializedFiles containing real Shader assets, for testing
VivifySerializedFile.cpp against a structure Unity would actually produce.

The point of these fixtures is that the parser is walking a genuine type tree --
flat depth-ordered nodes, a local string buffer, vector-wrapping-Array field
layout, alignment flags -- rather than a hardcoded guess at Unity 2021.3's
Shader layout.
"""
import struct

# ShaderCompilerPlatform
D3D11 = 4
GLES3PLUS = 9
VULKAN = 18
METAL = 14


class TypeTree:
    """Flat, depth-ordered type tree with a local string buffer."""

    def __init__(self):
        self.nodes = []          # (version, level, typeFlags, typeStr, nameStr, byteSize, index, metaFlag)
        self.strings = bytearray()
        self._offsets = {}

    def _str(self, text: str) -> int:
        if text in self._offsets:
            return self._offsets[text]
        offset = len(self.strings)
        self.strings += text.encode() + b"\0"
        self._offsets[text] = offset
        return offset

    def add(self, level, type_name, field_name, byte_size, *, is_array=False, align=False):
        self.nodes.append((
            1, level, 1 if is_array else 0,
            self._str(type_name), self._str(field_name),
            byte_size & 0xFFFFFFFF, len(self.nodes), 0x4000 if align else 0,
        ))
        return self

    def encode(self, sf_version: int) -> bytes:
        out = bytearray()
        out += struct.pack('<ii', len(self.nodes), len(self.strings))
        for (ver, level, flags, tstr, nstr, size, index, meta) in self.nodes:
            out += struct.pack('<HBBIIIII', ver, level, flags, tstr, nstr, size, index, meta)
            if sf_version >= 19:
                out += struct.pack('<Q', 0)   # ref type hash
        out += bytes(self.strings)
        return bytes(out)


def shader_type_tree(sf_version: int) -> TypeTree:
    """Mirrors the shape of UnityEngine.Shader: m_ParsedForm (with a string
    m_Name), then the platforms vector, then the blob vectors."""
    t = TypeTree()
    t.add(0, "Shader", "Base", -1)
    t.add(1, "SerializedShader", "m_ParsedForm", -1)
    t.add(2, "string", "m_Name", -1, align=True)
    t.add(3, "Array", "Array", -1, is_array=True, align=True)
    t.add(4, "int", "size", 4)
    t.add(4, "char", "data", 1)
    t.add(2, "unsigned int", "m_CustomEditorNameLen", 4)
    t.add(1, "vector", "platforms", -1)
    t.add(2, "Array", "Array", -1, is_array=True, align=True)
    t.add(3, "int", "size", 4)
    t.add(3, "unsigned int", "data", 4)
    t.add(1, "vector", "compressedBlob", -1)
    t.add(2, "Array", "Array", -1, is_array=True, align=True)
    t.add(3, "int", "size", 4)
    t.add(3, "UInt8", "data", 1)
    return t


def _align4(buf: bytearray):
    while len(buf) % 4:
        buf.append(0)


def shader_object(name: str, platforms, blob=b"\x01\x02\x03") -> bytes:
    """Serializes one Shader object matching shader_type_tree's layout."""
    out = bytearray()
    encoded = name.encode()
    out += struct.pack('<i', len(encoded)) + encoded
    _align4(out)                                    # m_Name align
    out += struct.pack('<I', 0)                     # m_CustomEditorNameLen
    out += struct.pack('<i', len(platforms))
    for p in platforms:
        out += struct.pack('<I', p)
    _align4(out)                                    # platforms Array align
    out += struct.pack('<i', len(blob)) + blob
    _align4(out)                                    # blob Array align
    return bytes(out)


def serialized_file_with_shaders(shaders, *, sf_version=21, target=19,
                                 unity="2021.3.16f1", enable_type_tree=True,
                                 extra_class_id=None):
    """shaders: list of (name, [platform, ...]). Returns the SerializedFile bytes."""
    tree = shader_type_tree(sf_version)

    types = bytearray()
    type_count = 1 + (1 if extra_class_id is not None else 0)
    types += struct.pack('<i', type_count)

    def emit_type(class_id, with_tree):
        buf = bytearray()
        buf += struct.pack('<i', class_id)
        if sf_version >= 16:
            buf += b'\x00'                          # isStrippedType
        if sf_version >= 17:
            buf += struct.pack('<h', -1)            # scriptTypeIndex
        if sf_version >= 13:
            if (sf_version < 16 and class_id < 0) or (sf_version >= 16 and class_id == 114):
                buf += bytes(16)
            buf += bytes(16)                        # old type hash
        if enable_type_tree:
            buf += with_tree.encode(sf_version) if with_tree else TypeTree().encode(sf_version)
            if sf_version >= 21:
                buf += struct.pack('<i', 0)         # type dependencies
        return bytes(buf)

    types += emit_type(48, tree)
    if extra_class_id is not None:
        other = TypeTree()
        other.add(0, "Mesh", "Base", -1)
        other.add(1, "unsigned int", "m_Dummy", 4)
        types += emit_type(extra_class_id, other)

    bodies = [shader_object(name, platforms) for (name, platforms) in shaders]
    if extra_class_id is not None:
        bodies.append(struct.pack('<I', 0xDEADBEEF))

    cursor = 0
    placements = []
    for body in bodies:
        while cursor % 8:
            cursor += 1
        placements.append((cursor, len(body)))
        cursor += len(body)

    # The metadata is assembled in one buffer because the object table's 4-byte
    # alignment is measured from the start of the *file*, not from the start of
    # the table -- getting that wrong silently shifts every object entry.
    head_len = 48 if sf_version >= 22 else 20
    meta = bytearray()
    meta += unity.encode() + b"\0"
    meta += struct.pack('<i', target)
    meta += b'\x01' if enable_type_tree else b'\x00'
    meta += types
    meta += struct.pack('<i', len(bodies))
    for i, (start, size) in enumerate(placements):
        while (head_len + len(meta)) % 4:
            meta.append(0)
        meta += struct.pack('<q', i + 1)             # pathID
        if sf_version >= 22:
            meta += struct.pack('<q', start)
        else:
            meta += struct.pack('<i', start)
        meta += struct.pack('<I', size)
        type_index = 1 if (extra_class_id is not None and i == len(bodies) - 1) else 0
        meta += struct.pack('<i', type_index)
    meta += struct.pack('<i', 0)                     # script types
    meta += struct.pack('<i', 0)                     # externals
    meta += struct.pack('<i', 0)                     # ref types
    meta += b'\0'                                    # user information

    metadata_size = len(meta)
    data_offset = head_len + metadata_size
    while data_offset % 16:
        data_offset += 1

    payload = bytearray()
    for (start, size), body in zip(placements, bodies):
        while len(payload) < start:
            payload.append(0)
        payload += body

    total = data_offset + len(payload)

    body = bytearray()
    if sf_version >= 22:
        body += struct.pack('>IIII', 0, 0, sf_version, 0)
        body += b'\x00\x00\x00\x00'
        body += struct.pack('>I', metadata_size)
        body += struct.pack('>Q', total)
        body += struct.pack('>Q', data_offset)
        body += struct.pack('>Q', 0)
    else:
        body += struct.pack('>I', metadata_size)
        body += struct.pack('>I', total)
        body += struct.pack('>I', sf_version)
        body += struct.pack('>I', data_offset)
        body += b'\x00\x00\x00\x00'
    body += meta
    while len(body) < data_offset:
        body.append(0)
    body += payload
    assert len(body) == total, (len(body), total)
    return bytes(body)
