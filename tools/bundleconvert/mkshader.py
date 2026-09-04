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

# ShaderGpuProgramType
GLES3 = 4
DX11_VERTEX_SM50 = 16
DX11_PIXEL_SM50 = 18


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
    m_Name), then the platforms vector, then the three nested length tables and
    the compressed blob.

    offsets/compressedLengths/decompressedLengths are vector<vector<uint>> as
    Unity 2019.3+ writes them -- a vector node wrapping an Array node whose
    element is itself a vector node wrapping an Array of uint. That double
    wrapping is exactly what the parser has to traverse without hardcoding, so
    the fixture reproduces it rather than flattening it."""
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
    for field in ("offsets", "compressedLengths", "decompressedLengths"):
        t.add(1, "vector", field, -1)
        t.add(2, "Array", "Array", -1, is_array=True, align=True)
        t.add(3, "int", "size", 4)
        t.add(3, "vector", "data", -1)
        t.add(4, "Array", "Array", -1, is_array=True, align=True)
        t.add(5, "int", "size", 4)
        t.add(5, "unsigned int", "data", 4)
    t.add(1, "vector", "compressedBlob", -1)
    t.add(2, "Array", "Array", -1, is_array=True, align=True)
    t.add(3, "int", "size", 4)
    t.add(3, "UInt8", "data", 1)
    return t


# --- LZ4 block format ------------------------------------------------------
#
# Only literal-run blocks are emitted. A block made entirely of literals is
# valid LZ4 -- the format does not require any matches -- and it keeps the
# fixtures readable, since the decompressed bytes appear verbatim in the file.
# lz4_block_with_match below covers the match path the literal case never
# reaches.

def lz4_literals(payload: bytes) -> bytes:
    """One all-literals LZ4 block. This is the last (and only) sequence, so it
    ends after its literals with no offset following."""
    out = bytearray()
    n = len(payload)
    token = (15 if n >= 15 else n) << 4
    out.append(token)
    if n >= 15:
        rest = n - 15
        while rest >= 255:
            out.append(255)
            rest -= 255
        out.append(rest)
    out += payload
    return bytes(out)


def lz4_block_with_match() -> tuple:
    """A hand-built block exercising the match-copy path: four literals, a
    back-reference that repeats them, then a final literal sequence.

    Returns (block, expected_plaintext)."""
    block = bytearray()
    block.append(0x40)            # 4 literals, match length 4 + 0
    block += b"ABCD"
    block += (4).to_bytes(2, "little")   # match offset 4
    block.append(0x10)            # final sequence: 1 literal, no match follows
    block += b"E"
    return bytes(block), b"ABCDABCDE"


def sub_program(program_type: int, code: bytes, *, blob_version=202012090,
                keywords=()) -> bytes:
    """One compiled sub-program, in the layout Unity writes: format version,
    ShaderGpuProgramType, three statistics ints, a fourth from 2016.08, then the
    aligned keyword strings and the program byte array."""
    out = bytearray()
    out += struct.pack('<ii', blob_version, program_type)
    out += bytes(12)                                  # statistics
    if blob_version >= 201608170:
        out += bytes(4)
    out += struct.pack('<i', len(keywords))
    for keyword in keywords:
        encoded = keyword.encode()
        out += struct.pack('<i', len(encoded)) + encoded
        _align4(out)
    if 201806140 <= blob_version < 202012090:
        out += struct.pack('<i', 0)                   # local keyword table
    out += struct.pack('<i', len(code)) + code
    return bytes(out)


def program_blob(programs, *, entry_size=8) -> bytes:
    """The container Unity puts inside one decompressed sub-blob: a count, then
    an [offset, length] table, then the sub-programs it points at."""
    header = 4 + len(programs) * entry_size
    body = bytearray()
    table = []
    for program in programs:
        table.append((header + len(body), len(program)))
        body += program
    out = bytearray()
    out += struct.pack('<I', len(programs))
    for (offset, length) in table:
        out += struct.pack('<II', offset, length)
        if entry_size == 12:
            out += struct.pack('<I', 0)
    out += body
    return bytes(out)


def _align4(buf: bytearray):
    while len(buf) % 4:
        buf.append(0)


def build_program_store(platform_blobs):
    """Lays out the compressed program store the way Unity does.

    platform_blobs is one list of decompressed sub-blobs per platform, in the
    same order as the shader's platforms array. Returns
    (offsets, compressed_lengths, decompressed_lengths, compressed_blob), where
    the three tables are nested one group per platform."""
    offsets, compressed_lengths, decompressed_lengths = [], [], []
    blob = bytearray()
    for sub_blobs in platform_blobs:
        group_offsets, group_compressed, group_decompressed = [], [], []
        for plain in sub_blobs:
            packed = lz4_literals(plain)
            group_offsets.append(len(blob))
            group_compressed.append(len(packed))
            group_decompressed.append(len(plain))
            blob += packed
        offsets.append(group_offsets)
        compressed_lengths.append(group_compressed)
        decompressed_lengths.append(group_decompressed)
    return offsets, compressed_lengths, decompressed_lengths, bytes(blob)


def shader_object(name: str, platforms, blob=b"\x01\x02\x03", *,
                  platform_blobs=None) -> bytes:
    """Serializes one Shader object matching shader_type_tree's layout.

    With platform_blobs given, a real program store is built and `blob` is
    ignored; without it the shader carries the same opaque three bytes it always
    has, which is what the platform-only tests want."""
    if platform_blobs is not None:
        offsets, compressed, decompressed, blob = build_program_store(platform_blobs)
    else:
        offsets = compressed = decompressed = [[] for _ in platforms]

    out = bytearray()
    encoded = name.encode()
    out += struct.pack('<i', len(encoded)) + encoded
    _align4(out)                                    # m_Name align
    out += struct.pack('<I', 0)                     # m_CustomEditorNameLen
    out += struct.pack('<i', len(platforms))
    for p in platforms:
        out += struct.pack('<I', p)
    _align4(out)                                    # platforms Array align

    for table in (offsets, compressed, decompressed):
        out += struct.pack('<i', len(table))
        for group in table:
            out += struct.pack('<i', len(group))
            for value in group:
                out += struct.pack('<I', value)
            _align4(out)                            # inner Array align
        _align4(out)                                # outer Array align

    out += struct.pack('<i', len(blob)) + blob
    _align4(out)                                    # blob Array align
    return bytes(out)


def texture_type_tree(sf_version: int) -> TypeTree:
    """Mirrors the shape of UnityEngine.Texture2D as far as the converter reads
    it: a string name, the size/format ints, the m_IsReadable bool, the
    StreamingInfo struct and the inline image data.

    Field order and presence differ between Unity versions, which is exactly why
    the parser finds every one of them by the name the tree gives it. A fixture
    that matched the parser's assumptions positionally would prove nothing.
    """
    t = TypeTree()
    t.add(0, "Texture2D", "Base", -1)
    t.add(1, "string", "m_Name", -1, align=True)
    t.add(2, "Array", "Array", -1, is_array=True)
    t.add(3, "int", "size", 4)
    t.add(3, "char", "data", 1)
    t.add(1, "int", "m_Width", 4)
    t.add(1, "int", "m_Height", 4)
    t.add(1, "int", "m_CompleteImageSize", 4)
    t.add(1, "int", "m_TextureFormat", 4)
    t.add(1, "int", "m_MipCount", 4)
    t.add(1, "bool", "m_IsReadable", 1)
    t.add(1, "bool", "m_IsPreProcessed", 1, align=True)
    t.add(1, "StreamingInfo", "m_StreamData", -1)
    t.add(2, "unsigned int", "offset", 4)
    t.add(2, "unsigned int", "size", 4)
    t.add(2, "string", "path", -1, align=True)
    t.add(3, "Array", "Array", -1, is_array=True)
    t.add(4, "int", "size", 4)
    t.add(4, "char", "data", 1)
    t.add(1, "TypelessData", "image data", -1, is_array=True)
    t.add(2, "int", "size", 4)
    t.add(2, "UInt8", "data", 1)
    return t


def texture_object(name="tex", *, width=4, height=4, texture_format=10, mip_count=1,
                   is_readable=False, image_data=b"", stream_size=0, stream_path=""):
    """One serialized Texture2D body, laid out to match texture_type_tree."""
    body = bytearray()

    def put_string(text: bytes):
        body.extend(struct.pack('<i', len(text)))
        body.extend(text)
        _align4(body)

    put_string(name.encode())
    body.extend(struct.pack('<iiiii', width, height,
                            len(image_data) or stream_size, texture_format, mip_count))
    body.append(1 if is_readable else 0)
    body.append(0)                                   # m_IsPreProcessed
    _align4(body)
    body.extend(struct.pack('<II', 0, stream_size))  # StreamingInfo offset, size
    put_string(stream_path.encode())
    body.extend(struct.pack('<i', len(image_data)))
    body.extend(image_data)
    return bytes(body)


def serialized_file_with_shaders(shaders, *, sf_version=21, target=19,
                                 unity="2021.3.16f1", enable_type_tree=True,
                                 extra_class_id=None, textures=None):
    """shaders: list of (name, [platform, ...]). textures: list of kwargs for
    texture_object. Returns the SerializedFile bytes."""
    textures = list(textures or [])
    tree = shader_type_tree(sf_version)

    types = bytearray()
    type_count = 1 + (1 if textures else 0) + (1 if extra_class_id is not None else 0)
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
    texture_type_index = None
    if textures:
        texture_type_index = 1
        types += emit_type(28, texture_type_tree(sf_version))
    extra_type_index = None
    if extra_class_id is not None:
        extra_type_index = 1 + (1 if textures else 0)
        other = TypeTree()
        other.add(0, "Mesh", "Base", -1)
        other.add(1, "unsigned int", "m_Dummy", 4)
        types += emit_type(extra_class_id, other)

    # A fixture entry is (name, platforms) or (name, platforms, platform_blobs).
    bodies = [shader_object(entry[0], entry[1],
                            platform_blobs=entry[2] if len(entry) > 2 else None)
              for entry in shaders]
    type_indices = [0] * len(bodies)
    for spec in textures:
        bodies.append(texture_object(**spec))
        type_indices.append(texture_type_index)
    if extra_class_id is not None:
        bodies.append(struct.pack('<I', 0xDEADBEEF))
        type_indices.append(extra_type_index)

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
        meta += struct.pack('<i', type_indices[i])
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
