#!/usr/bin/env python3
"""Builds DXBC containers for testing the DXBC -> GLSL translator.

There is no DirectX shader compiler on the machines this repo is developed and
built on, and a Quest cannot produce DXBC either. So the fixtures are assembled
here: real container framing, real RDEF/ISGN/OSGN chunks, and a real Shader
Model 4/5 token stream, written from the format documentation rather than
captured from fxc.

That is a genuine limit and worth stating plainly: these fixtures prove the
decoder reads what the format says, not that it reads what Microsoft's compiler
happens to emit. What they do catch is every framing, bounds and translation
error, which is the class of bug that would corrupt a bundle.
"""
import struct

# ---------------------------------------------------------------------------
# Operand encoding
# ---------------------------------------------------------------------------

OPERAND_TEMP = 0
OPERAND_INPUT = 1
OPERAND_OUTPUT = 2
OPERAND_INDEXABLE_TEMP = 3
OPERAND_IMMEDIATE32 = 4
OPERAND_SAMPLER = 6
OPERAND_RESOURCE = 7
OPERAND_CONSTANT_BUFFER = 8
OPERAND_IMMEDIATE_CONSTANT_BUFFER = 9
OPERAND_OUTPUT_DEPTH = 12
OPERAND_NULL = 13
OPERAND_LABEL = 10
OPERAND_UNORDERED_ACCESS_VIEW = 30
OPERAND_THREAD_GROUP_SHARED_MEMORY = 31
OPERAND_INPUT_THREAD_ID = 32
OPERAND_INPUT_THREAD_GROUP_ID = 33
OPERAND_INPUT_THREAD_ID_IN_GROUP = 34

MOD_NONE = 0
MOD_NEG = 1
MOD_ABS = 2
MOD_ABSNEG = 3


def _operand(op_type, indices, four_component=True, mask=None, swizzle=None,
             modifier=MOD_NONE, immediates=None):
    """One operand token plus whatever index/immediate dwords follow it."""
    token = 0
    if four_component:
        token |= 2  # D3D10_SB_OPERAND_4_COMPONENT
        if mask is not None:
            token |= 0 << 2  # mask mode
            token |= (mask & 0xF) << 4
        else:
            sw = swizzle if swizzle is not None else (0, 1, 2, 3)
            token |= 1 << 2  # swizzle mode
            packed = sw[0] | (sw[1] << 2) | (sw[2] << 4) | (sw[3] << 6)
            token |= packed << 4
    else:
        token |= 1  # 1-component
    token |= (op_type & 0xFF) << 12
    token |= (len(indices) & 0x3) << 20
    for i in range(len(indices)):
        token |= 0 << (22 + i * 3)  # every index here is an immediate32
    extended = []
    if modifier != MOD_NONE:
        token |= 0x80000000
        extended.append(1 | (modifier << 6))

    dwords = [token] + extended
    if op_type == OPERAND_IMMEDIATE32:
        dwords += list(immediates or [])
    else:
        dwords += [int(i) & 0xFFFFFFFF for i in indices]
    return dwords


def dest(op_type, index, mask=0xF):
    return _operand(op_type, [index] if index is not None else [], mask=mask)


def dest_cb(buffer_index, row, mask=0xF):
    return _operand(OPERAND_CONSTANT_BUFFER, [buffer_index, row], mask=mask)


def src(op_type, index, swizzle=(0, 1, 2, 3), modifier=MOD_NONE):
    indices = [index] if index is not None else []
    return _operand(op_type, indices, swizzle=swizzle, modifier=modifier)


def src_cb(buffer_index, row, swizzle=(0, 1, 2, 3), modifier=MOD_NONE):
    return _operand(OPERAND_CONSTANT_BUFFER, [buffer_index, row], swizzle=swizzle,
                    modifier=modifier)


def src_icb(row, swizzle=(0, 1, 2, 3)):
    return _operand(OPERAND_IMMEDIATE_CONSTANT_BUFFER, [row], swizzle=swizzle)


def src_indexable(array, row, swizzle=(0, 1, 2, 3)):
    return _operand(OPERAND_INDEXABLE_TEMP, [array, row], swizzle=swizzle)


def src_null():
    return _operand(OPERAND_NULL, [], four_component=False)


def imm_float(*values):
    bits = [struct.unpack("<I", struct.pack("<f", float(v)))[0] for v in values]
    while len(bits) < 4:
        bits.append(bits[-1] if bits else 0)
    return _operand(OPERAND_IMMEDIATE32, [], immediates=bits)


def imm_int(*values):
    bits = [v & 0xFFFFFFFF for v in values]
    while len(bits) < 4:
        bits.append(bits[-1] if bits else 0)
    return _operand(OPERAND_IMMEDIATE32, [], immediates=bits)


# ---------------------------------------------------------------------------
# Instruction encoding
# ---------------------------------------------------------------------------

def insn(opcode, *operands, controls=0, saturate=False, extra=()):
    payload = []
    for operand in operands:
        payload += operand
    payload += list(extra)
    length = 1 + len(payload)
    if length > 0x7F:
        raise ValueError("instruction too long for a 7-bit length field")
    token = (opcode & 0x7FF) | ((controls & 0x1FFF) << 11) | (length << 24)
    if saturate:
        token |= 1 << 13
    return [token] + payload


def custom_data(class_id, dwords):
    token = (53 & 0x7FF) | ((class_id & 0x1FFFFF) << 11)
    return [token, len(dwords) + 2] + list(dwords)


def immediate_constant_buffer(rows):
    """rows: a list of 4-float tuples."""
    dwords = []
    for row in rows:
        for value in row:
            dwords.append(struct.unpack("<I", struct.pack("<f", float(value)))[0])
    return custom_data(3, dwords)


# ---------------------------------------------------------------------------
# Chunks
# ---------------------------------------------------------------------------

def shex_chunk(instructions, stage=1, major=5, minor=0, four_cc=b"SHEX"):
    """stage: 0 pixel, 1 vertex, ... (the D3D10 program-type numbering)."""
    dwords = []
    for instruction in instructions:
        dwords += instruction
    version = (minor & 0xF) | ((major & 0xF) << 4) | ((stage & 0xFFFF) << 16)
    body = struct.pack("<II", version, len(dwords) + 2)
    body += b"".join(struct.pack("<I", d) for d in dwords)
    return (four_cc, body)


class _Pool:
    """A string pool that hands back chunk-relative offsets."""

    def __init__(self, base):
        self.base = base
        self.data = bytearray()
        self.seen = {}

    def add(self, text):
        if text in self.seen:
            return self.seen[text]
        offset = self.base + len(self.data)
        self.data += text.encode("utf-8") + b"\x00"
        self.seen[text] = offset
        return offset


def signature_chunk(elements, four_cc=b"ISGN"):
    """elements: dicts with name/index/sv/component_type/register/mask/rw_mask."""
    count = len(elements)
    header = struct.pack("<II", count, 8)
    body_size = 8 + count * 24
    pool = _Pool(body_size)
    entries = b""
    for element in elements:
        name_offset = pool.add(element["name"])
        entries += struct.pack(
            "<IIIIIBBH",
            name_offset,
            element.get("index", 0),
            element.get("sv", 0),
            element.get("component_type", 3),
            element.get("register", 0),
            element.get("mask", 0xF),
            element.get("rw_mask", 0xF),
            0,
        )
    return (four_cc, header + entries + bytes(pool.data))


def rdef_chunk(constant_buffers=(), bindings=(), creator="vivify-quest test", major=5):
    """constant_buffers: dicts with name/size/variables; bindings: dicts with
    name/type/dimension/bind_point."""
    variable_stride = 40 if major >= 5 else 24
    header_size = 28

    binding_count = len(bindings)
    binding_offset = header_size
    cb_offset = binding_offset + binding_count * 32

    variable_blocks = []
    cursor = cb_offset + len(constant_buffers) * 24
    for buffer in constant_buffers:
        variable_blocks.append(cursor)
        cursor += len(buffer["variables"]) * variable_stride
    type_offset_base = cursor
    type_records = []
    for buffer in constant_buffers:
        for _ in buffer["variables"]:
            type_records.append(None)
    cursor += len(type_records) * 16

    pool = _Pool(cursor)

    creator_offset = pool.add(creator)
    binding_bytes = b""
    for binding in bindings:
        binding_bytes += struct.pack(
            "<IIIIIIII",
            pool.add(binding["name"]),
            binding.get("type", 2),
            binding.get("return_type", 5),
            binding.get("dimension", 4),
            binding.get("sample_count", 0),
            binding.get("bind_point", 0),
            binding.get("bind_count", 1),
            0,
        )

    cb_bytes = b""
    variable_bytes = b""
    type_bytes = b""
    type_index = 0
    for index, buffer in enumerate(constant_buffers):
        cb_bytes += struct.pack(
            "<IIIIII",
            pool.add(buffer["name"]),
            len(buffer["variables"]),
            variable_blocks[index],
            buffer["size"],
            0,
            0,
        )
        for variable in buffer["variables"]:
            record_offset = type_offset_base + type_index * 16
            type_index += 1
            fields = [
                pool.add(variable["name"]),
                variable["offset"],
                variable["size"],
                2,  # used
                record_offset,
                0,
            ]
            variable_bytes += struct.pack("<IIIIII", *fields)
            if variable_stride == 40:
                variable_bytes += struct.pack("<IIII", 0xFFFFFFFF, 0, 0xFFFFFFFF, 0)
            type_bytes += struct.pack(
                "<HHHHHHI",
                variable.get("class", 1),
                variable.get("type", 3),
                variable.get("rows", 1),
                variable.get("columns", 4),
                variable.get("elements", 0),
                0,
                0,
            )

    header = struct.pack(
        "<IIIIIII",
        len(constant_buffers),
        cb_offset,
        binding_count,
        binding_offset,
        0x0500 if major >= 5 else 0x0400,
        0,
        creator_offset,
    )
    body = header + binding_bytes + cb_bytes + variable_bytes + type_bytes + bytes(pool.data)
    return (b"RDEF", body)


def unity_program(chunks, prefix=None):
    """A DXBC container behind a Unity-style program header.

    A D3D11 sub-program in a Unity shader blob is not a bare container: Unity
    writes its own binding header first and the container follows it. Requiring
    the magic at offset zero rejected every shader in every real bundle, so the
    fixtures cover the prefixed form as well as the bare one."""
    if prefix is None:
        # Shaped like Unity's: a few counts and offsets, no DXBC magic in it.
        prefix = struct.pack("<IIIIII", 1, 0, 2, 0x30, 0, 0)
    return prefix + container(chunks)


def container(chunks):
    """chunks: a list of (fourcc, body) pairs."""
    header_size = 32 + len(chunks) * 4
    offsets = []
    payload = b""
    cursor = header_size
    for four_cc, body in chunks:
        offsets.append(cursor)
        payload += four_cc + struct.pack("<I", len(body)) + body
        cursor += 8 + len(body)
    total = header_size + len(payload)
    out = b"DXBC" + b"\x00" * 16 + struct.pack("<III", 1, total, len(chunks))
    out += b"".join(struct.pack("<I", offset) for offset in offsets)
    return out + payload
