#!/usr/bin/env python3
"""Tests for the DXBC -> GLSL ES translator.

Builds DXBC containers (see mkdxbc.py) and checks what the translator makes of
them. Run under ASan/UBSan: shader bytecode arrives inside a downloaded map, so
the corruption pass matters as much as the happy path.

  python3 tools/dxbc/run_tests.py [path-to-dxbctool-binary]
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import mkdxbc as m  # noqa: E402

BINARY = sys.argv[1] if len(sys.argv) > 1 else "/tmp/dxbctool"

passed = 0
failed = 0
tmpdir = tempfile.mkdtemp(prefix="dxbc-")

# Opcodes used by the fixtures, by their D3D10_SB_OPCODE_TYPE numbers.
ADD, AND, BREAK, DISCARD, DIV, DP3, ELSE, ENDIF, ENDLOOP = 0, 1, 2, 13, 14, 16, 18, 21, 22
EQ, EXP, FRC, FTOI, GE, IADD, IF, IMAD, ISHL, ITOF = 24, 25, 26, 27, 29, 30, 31, 35, 41, 43
LD, LOG, LOOP, LT, MAD, MIN, MAX, MOV, MOVC, MUL = 45, 47, 48, 49, 50, 51, 52, 54, 55, 56
NE, RET, RSQ, SAMPLE, SAMPLE_L, SQRT, SINCOS, UTOF = 57, 62, 68, 69, 72, 75, 77, 86
DCL_RESOURCE, DCL_CONSTANT_BUFFER, DCL_SAMPLER = 88, 89, 90
DCL_INPUT, DCL_INPUT_PS, DCL_OUTPUT, DCL_OUTPUT_SIV = 95, 98, 101, 103
DCL_TEMPS, DCL_INDEXABLE_TEMP, DCL_GLOBAL_FLAGS = 104, 105, 106
GATHER4, RCP, RESINFO, SWAPC = 109, 129, 61, 142

X, Y, Z, W = 0, 1, 2, 3
TEMP, INPUT, OUTPUT, RESOURCE, SAMPLER_T = (
    m.OPERAND_TEMP, m.OPERAND_INPUT, m.OPERAND_OUTPUT, m.OPERAND_RESOURCE, m.OPERAND_SAMPLER)


def check(name, condition, detail=""):
    global passed, failed
    if condition:
        passed += 1
    else:
        failed += 1
        print("FAIL  %s%s" % (name, ("  -- " + detail) if detail else ""))


def run(mode, data, name="case.dxbc"):
    path = os.path.join(tmpdir, name)
    with open(path, "wb") as handle:
        handle.write(data)
    proc = subprocess.run([BINARY, mode, path], capture_output=True, timeout=60)
    text = proc.stdout.decode("utf-8", errors="replace")
    head, _, source = text.partition("---\n")
    fields = {}
    lists = {}
    for line in head.splitlines():
        key, _, value = line.partition("=")
        lists.setdefault(key, []).append(value)
        fields.setdefault(key, value)
    return proc, fields, lists, source


# ---------------------------------------------------------------------------
# Fixtures
# ---------------------------------------------------------------------------

def globals_cbuffer(variables, size):
    return m.rdef_chunk(
        constant_buffers=[{"name": "$Globals", "size": size, "variables": variables}],
        bindings=[{"name": "$Globals", "type": 0, "dimension": 0, "bind_point": 0}])


def rdef_with_texture(variables, size, textures):
    bindings = [{"name": "$Globals", "type": 0, "dimension": 0, "bind_point": 0}]
    for index, (name, dimension) in enumerate(textures):
        bindings.append({"name": name, "type": 2, "dimension": dimension, "bind_point": index})
    return m.rdef_chunk(
        constant_buffers=[{"name": "$Globals", "size": size, "variables": variables}],
        bindings=bindings)


VECTOR4 = {"class": 1, "type": 3, "rows": 1, "columns": 4}
MATRIX4 = {"class": 3, "type": 3, "rows": 4, "columns": 4}
SCALAR = {"class": 0, "type": 3, "rows": 1, "columns": 1}
SCALAR_INT = {"class": 0, "type": 2, "rows": 1, "columns": 1}


def variable(name, offset, size, shape, elements=0):
    entry = dict(shape)
    entry.update({"name": name, "offset": offset, "size": size, "elements": elements})
    return entry


def simple_vertex():
    isgn = m.signature_chunk([
        {"name": "POSITION", "index": 0, "register": 0},
        {"name": "TEXCOORD", "index": 0, "register": 1, "mask": 0x3, "rw_mask": 0x3},
    ], b"ISGN")
    osgn = m.signature_chunk([
        {"name": "SV_POSITION", "index": 0, "register": 0, "sv": 1, "rw_mask": 0},
        {"name": "TEXCOORD", "index": 0, "register": 1, "mask": 0x3, "rw_mask": 0xC},
    ], b"OSGN")
    rdef = globals_cbuffer([
        variable("unity_MatrixVP", 0, 64, MATRIX4),
        variable("_MainTex_ST", 64, 16, VECTOR4),
    ], 80)
    code = []
    code += m.insn(DCL_GLOBAL_FLAGS, controls=1)
    code += m.insn(DCL_CONSTANT_BUFFER, m.src_cb(0, 5))
    code += m.insn(DCL_INPUT, m.dest(INPUT, 0))
    code += m.insn(DCL_INPUT, m.dest(INPUT, 1, 0x3))
    code += m.insn(DCL_OUTPUT_SIV, m.dest(OUTPUT, 0), extra=[1])
    code += m.insn(DCL_OUTPUT, m.dest(OUTPUT, 1, 0x3))
    code += m.insn(DCL_TEMPS, extra=[1])
    code += m.insn(MUL, m.dest(TEMP, 0), m.src(INPUT, 0, (X, X, X, X)), m.src_cb(0, 0))
    code += m.insn(MAD, m.dest(TEMP, 0), m.src(INPUT, 0, (Y, Y, Y, Y)), m.src_cb(0, 1),
                   m.src(TEMP, 0))
    code += m.insn(MAD, m.dest(TEMP, 0), m.src(INPUT, 0, (Z, Z, Z, Z)), m.src_cb(0, 2),
                   m.src(TEMP, 0))
    code += m.insn(ADD, m.dest(OUTPUT, 0), m.src(TEMP, 0), m.src_cb(0, 3))
    code += m.insn(MAD, m.dest(OUTPUT, 1, 0x3), m.src(INPUT, 1, (X, Y, X, X)),
                   m.src_cb(0, 4, (X, Y, X, X)), m.src_cb(0, 4, (Z, W, Z, Z)))
    code += m.insn(RET)
    return m.container([rdef, isgn, osgn, m.shex_chunk([code], stage=1)])


def pixel_shader(body, variables=(variable("_Color", 0, 16, VECTOR4),), size=16,
                 textures=(("_MainTex", 4),), inputs=None, outputs=None, temps=1):
    isgn = m.signature_chunk(inputs if inputs is not None else [
        {"name": "TEXCOORD", "index": 0, "register": 0, "mask": 0x3, "rw_mask": 0x3},
    ], b"ISGN")
    osgn = m.signature_chunk(outputs if outputs is not None else [
        {"name": "SV_Target", "index": 0, "register": 0, "rw_mask": 0},
    ], b"OSGN")
    rdef = rdef_with_texture(list(variables), size, list(textures))
    code = []
    code += m.insn(DCL_GLOBAL_FLAGS, controls=1)
    code += m.insn(DCL_CONSTANT_BUFFER, m.src_cb(0, (size + 15) // 16))
    for index, _ in enumerate(textures):
        code += m.insn(DCL_SAMPLER, m.dest(SAMPLER_T, index))
        code += m.insn(DCL_RESOURCE, m.dest(RESOURCE, index), extra=[0x5555])
    code += m.insn(DCL_INPUT_PS, m.dest(INPUT, 0, 0x3), controls=(2 << 0))
    code += m.insn(DCL_OUTPUT, m.dest(OUTPUT, 0))
    if temps:
        code += m.insn(DCL_TEMPS, extra=[temps])
    code += body
    code += m.insn(RET)
    return m.container([rdef, isgn, osgn, m.shex_chunk([code], stage=0)])


# ---------------------------------------------------------------------------
# Container and reflection
# ---------------------------------------------------------------------------

vertex = simple_vertex()
proc, fields, lists, _ = run("parse", vertex)
check("vertex parses", fields.get("ok") == "1", fields.get("error", ""))
check("vertex stage", fields.get("stage") == "vertex", fields.get("stage", ""))
check("vertex version", fields.get("version") == "5.0", fields.get("version", ""))
check("vertex temps", fields.get("temps") == "1", fields.get("temps", ""))
check("vertex creator", fields.get("creator") == "vivify-quest test", fields.get("creator", ""))
check("vertex inputs", len(lists.get("input", [])) == 2, str(lists.get("input")))
check("vertex outputs", len(lists.get("output", [])) == 2, str(lists.get("output")))
check("vertex cbuffer", fields.get("cbuffer", "").startswith("$Globals size=80 bind=0"),
      fields.get("cbuffer", ""))
check("vertex cbvars", len(lists.get("cbvar", [])) == 2, str(lists.get("cbvar")))
check("matrix reflected", "unity_MatrixVP offset=0 size=64 class=3" in lists.get("cbvar", [""])[0],
      str(lists.get("cbvar")))

proc, fields, _, _ = run("disasm", vertex)
disassembly = proc.stdout.decode("utf-8", errors="replace")
expected = """ok=1
vertex_5_0
dcl_globalFlags
dcl_constantbuffer cb0[5]
dcl_input v[0]
dcl_input v[1].xy
dcl_output_siv o[0]
dcl_output o[1].xy
dcl_temps 1
mul r[0], v[0].xxxx, cb0[0]
mad r[0], v[0].yyyy, cb0[1], r[0]
mad r[0], v[0].zzzz, cb0[2], r[0]
add o[0], r[0], cb0[3]
mad o[1].xy, v[1].xyxx, cb0[4].xyxx, cb0[4].zwzz
ret
"""
check("vertex disassembly", disassembly == expected, repr(disassembly))

proc, fields, lists, source = run("glsl", vertex)
check("vertex translates", fields.get("ok") == "1", fields.get("error", ""))
check("vertex version line", source.startswith("#version 300 es\n"), source[:40])
check("attribute naming", "in vec4 in_POSITION0;" in source, source)
check("varying naming", "out vec4 vs_TEXCOORD0;" in source, source)
check("matrix uniform naming", "uniform vec4 hlslcc_mtx4x4unity_MatrixVP[4];" in source, source)
check("clip position", "gl_Position.xyzw = (r0.xyzw + hlslcc_mtx4x4unity_MatrixVP[3].xyzw);" in source,
      source)
check("swizzle collapse", "_MainTex_ST.xy + _MainTex_ST.zw" in source, source)
check("temp declared", "vec4 r0;" in source, source)
check("uniforms listed", lists.get("uniform") == ["hlslcc_mtx4x4unity_MatrixVP", "_MainTex_ST"],
      str(lists.get("uniform")))
check("no samplers listed", "sampler" not in lists, str(lists.get("sampler")))

# ---------------------------------------------------------------------------
# Fragment shaders
# ---------------------------------------------------------------------------

body = []
body += m.insn(SAMPLE, m.dest(TEMP, 0), m.src(INPUT, 0), m.src(RESOURCE, 0), m.src(SAMPLER_T, 0))
body += m.insn(MUL, m.dest(OUTPUT, 0), m.src(TEMP, 0), m.src_cb(0, 0), saturate=True)
proc, fields, lists, source = run("glsl", pixel_shader(body))
check("texture translates", fields.get("ok") == "1", fields.get("error", ""))
check("sampler declared", "uniform highp sampler2D _MainTex;" in source, source)
check("texture call", "texture(_MainTex, vs_TEXCOORD0.xy).xyzw" in source, source)
check("saturate wraps", "clamp((r0.xyzw * _Color.xyzw), 0.0, 1.0)" in source, source)
check("fragment output", "layout(location = 0) out vec4 SV_Target0;" in source, source)
check("samplers listed", lists.get("sampler") == ["_MainTex"], str(lists.get("sampler")))

# A masked write must only evaluate the components it writes.
body = m.insn(MUL, m.dest(OUTPUT, 0, 0x6), m.src(INPUT, 0, (Y, Z, W, X)), m.src_cb(0, 0))
proc, fields, _, source = run("glsl", pixel_shader(body, temps=0))
check("masked write", "SV_Target0.yz = (vs_TEXCOORD0.zw * _Color.yz);" in source, source)

# Comparisons produce D3D's all-bits-set, not a GLSL bool.
body = m.insn(LT, m.dest(TEMP, 0, 0x3), m.src(INPUT, 0), m.src_cb(0, 0))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("vector compare", "intBitsToFloat(-ivec2(lessThan(vs_TEXCOORD0.xy, _Color.xy)))" in source,
      source)

body = m.insn(GE, m.dest(TEMP, 0, 0x1), m.src(INPUT, 0), m.src_cb(0, 0))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("scalar compare", "intBitsToFloat((vs_TEXCOORD0.x >= _Color.x) ? -1 : 0)" in source, source)

# movc selects per component off an integer-typed condition.
body = m.insn(MOVC, m.dest(OUTPUT, 0, 0x3), m.src(TEMP, 0), m.src_cb(0, 0), m.src(INPUT, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("vector movc",
      "mix(vs_TEXCOORD0.xy, _Color.xy, notEqual(floatBitsToInt(r0.xy), ivec2(0)))" in source, source)
body = m.insn(MOVC, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0), m.src_cb(0, 0), m.src(INPUT, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("scalar movc", "((floatBitsToInt(r0.x) != 0) ? _Color.x : vs_TEXCOORD0.x)" in source, source)

# Integer maths round-trips through the bit-cast form.
body = m.insn(IADD, m.dest(TEMP, 0, 0x1), m.src(TEMP, 0), m.imm_int(3) if False else m.imm_int(3))
body += m.insn(ITOF, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("integer add", "intBitsToFloat(floatBitsToInt(r0.x) + 3)" in source, source)
check("int to float", "SV_Target0.x = float(floatBitsToInt(r0.x));" in source, source)

# Source modifiers.
body = m.insn(ADD, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0, modifier=m.MOD_NEG),
              m.src_cb(0, 0, modifier=m.MOD_ABS))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("negate modifier", "(-r0.x)" in source, source)
check("absolute modifier", "abs(_Color.x)" in source, source)

# Control flow.
body = m.insn(IF, m.src(TEMP, 0), controls=(1 << 7))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src_cb(0, 0))
body += m.insn(ELSE)
body += m.insn(DISCARD, m.src(TEMP, 0), controls=(1 << 7))
body += m.insn(ENDIF)
proc, fields, _, source = run("glsl", pixel_shader(body))
check("if emitted", "if (floatBitsToInt(r0.x) != 0) {" in source, source)
check("else emitted", "} else {" in source, source)
check("discard emitted", "if (floatBitsToInt(r0.x) != 0) discard;" in source, source)

body = m.insn(LOOP)
body += m.insn(ADD, m.dest(TEMP, 0), m.src(TEMP, 0), m.imm_float(1.0))
body += m.insn(BREAK)
body += m.insn(ENDLOOP)
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("loop emitted", "while (true) {" in source, source)
check("break emitted", "break;" in source, source)
check("float literal", "vec4(1, 1, 1, 1)" in source or "1.0" in source, source)

# rcp, sincos and the two-destination shape.
body = m.insn(RCP, m.dest(TEMP, 0, 0x3), m.src(INPUT, 0))
body += m.insn(SINCOS, m.dest(TEMP, 0, 0x1), m.src_null(), m.src(INPUT, 0))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("reciprocal", "(vec2(1.0) / vs_TEXCOORD0.xy)" in source, source)
check("sincos sin only", "sin(vs_TEXCOORD0.x)" in source and "cos(" not in source, source)

# Scalar cbuffer variables get their own uniform, and packing is respected.
variables = (variable("_Color", 0, 16, VECTOR4), variable("_Cutoff", 16, 4, SCALAR),
             variable("_Fade", 20, 4, SCALAR))
body = m.insn(ADD, m.dest(OUTPUT, 0, 0x1), m.src_cb(0, 1, (X, X, X, X)),
              m.src_cb(0, 1, (Y, Y, Y, Y)))
proc, fields, lists, source = run("glsl", pixel_shader(body, variables=variables, size=32, temps=0))
check("scalar uniforms", "uniform float _Cutoff;" in source and "uniform float _Fade;" in source,
      source)
check("scalar packing", "(_Cutoff + _Fade)" in source, source)

# An integer-typed cbuffer variable is read as bits, not converted.
variables = (variable("_Mode", 0, 4, SCALAR_INT),)
body = m.insn(ITOF, m.dest(OUTPUT, 0, 0x1), m.src_cb(0, 0, (X, X, X, X)))
proc, fields, _, source = run("glsl", pixel_shader(body, variables=variables, size=16, temps=0))
check("int uniform declared", "uniform int _Mode;" in source, source)
check("int uniform read as bits", "float(floatBitsToInt(intBitsToFloat(_Mode)))" in source, source)

# Array variables and dynamic indexing.
variables = (variable("_Points", 0, 128, VECTOR4, elements=8),)
dynamic = m._operand(m.OPERAND_CONSTANT_BUFFER, [0, 2], swizzle=(X, Y, Z, W))
# Rebuild the row index as relative: index representation 3 (immediate + relative).
token = dynamic[0] & ~(0x7 << 25)
token |= 3 << 25
dynamic = [token, 0, 2] + m.src(TEMP, 0, (X, X, X, X))
body = m.insn(MOV, m.dest(OUTPUT, 0), dynamic)
proc, fields, _, source = run("glsl", pixel_shader(body, variables=variables, size=128))
check("array uniform", "uniform vec4 _Points[8];" in source, source)
check("dynamic index", "_Points[floatBitsToInt(r0.x) + 2].xyzw" in source, source)

# The same dynamic index against a non-array variable has no named form.
variables = (variable("_Color", 0, 16, VECTOR4), variable("_Tint", 16, 16, VECTOR4),
             variable("_Rim", 32, 16, VECTOR4))
proc, fields, _, source = run("glsl", pixel_shader(body, variables=variables, size=48))
check("dynamic index refused", fields.get("ok") == "0", source)
check("dynamic index reason", "not an array" in fields.get("error", ""), fields.get("error", ""))

# Immediate constant buffers.
body = m.immediate_constant_buffer([(1.0, 2.0, 3.0, 4.0), (5.0, 6.0, 7.0, 8.0)])
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src_icb(1))
proc, fields, _, source = run("glsl", pixel_shader(body, temps=0))
check("immediate cbuffer", "const vec4 ImmCB[2] = vec4[2](" in source, source)
check("immediate cbuffer read", "ImmCB[1].xyzw" in source, source)

# Texture dimensions.
body = m.insn(SAMPLE, m.dest(OUTPUT, 0), m.src(INPUT, 0), m.src(RESOURCE, 0), m.src(SAMPLER_T, 0))
proc, fields, _, source = run("glsl", pixel_shader(body, textures=(("_Cube", 9),), temps=0))
check("cube sampler", "uniform highp samplerCube _Cube;" in source, source)
check("cube coordinates", "texture(_Cube, vs_TEXCOORD0.xyz)" in source, source)

proc, fields, _, source = run("glsl", pixel_shader(body, textures=(("_Volume", 8),), temps=0))
check("3d sampler", "uniform highp sampler3D _Volume;" in source, source)

# A buffer texture has no sampler state and no mip chain: GLSL ES can only
# fetch it by texel, so a filtered fetch from one is refused rather than
# emitted as a texture() call that would not compile.
proc, fields, _, source = run("glsl", pixel_shader(body, textures=(("_Buffer", 1),), temps=0))
check("filtered fetch from a buffer texture refused", fields.get("ok") == "0", source)
check("filtered fetch reason names the texture", "_Buffer" in fields.get("error", ""),
      fields.get("error", ""))

proc, fields, _, source = run("glsl", pixel_shader(body, textures=(("_MS", 6),), temps=0))
check("filtered fetch from a multisample texture refused", fields.get("ok") == "0", source)

# sample_l carries its lod through.
body = m.insn(SAMPLE_L, m.dest(OUTPUT, 0), m.src(INPUT, 0), m.src(RESOURCE, 0),
              m.src(SAMPLER_T, 0), m.imm_float(2.0))
proc, fields, _, source = run("glsl", pixel_shader(body, temps=0))
check("sample_l", "textureLod(_MainTex, vs_TEXCOORD0.xy, 2.0)" in source, source)

# The resource operand's swizzle decides which channel lands where.
body = m.insn(SAMPLE, m.dest(OUTPUT, 0, 0x1), m.src(INPUT, 0),
              m.src(RESOURCE, 0, (W, W, W, W)), m.src(SAMPLER_T, 0))
proc, fields, _, source = run("glsl", pixel_shader(body, temps=0))
check("resource swizzle", "texture(_MainTex, vs_TEXCOORD0.xy).w" in source, source)

# ---------------------------------------------------------------------------
# Refusals
# ---------------------------------------------------------------------------

MSAD = 213
body = m.insn(MSAD, m.dest(TEMP, 0), m.src(TEMP, 0), m.src(TEMP, 0), m.src(TEMP, 0))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, _ = run("glsl", pixel_shader(body))
check("unsupported opcode refused", fields.get("ok") == "0", fields.get("error", ""))
check("unsupported opcode named", "msad" in fields.get("error", ""), fields.get("error", ""))

DADD = 191
body = m.insn(DADD, m.dest(TEMP, 0), m.src(TEMP, 0), m.src(TEMP, 0))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, _ = run("glsl", pixel_shader(body))
check("double precision refused", fields.get("ok") == "0", fields.get("error", ""))
check("double precision reason", "double" in fields.get("error", ""), fields.get("error", ""))

# Tessellation is the one stage left untranslated, and on purpose: a hull
# program is several instruction streams with their own register spaces, and
# getting it wrong is worse than not doing it.
hull = m.container([m.shex_chunk([m.insn(RET)], stage=3)])
proc, fields, _, _ = run("glsl", hull)
check("hull refused", fields.get("ok") == "0", fields.get("error", ""))
check("hull reason", "tessellation" in fields.get("error", ""), fields.get("error", ""))

sm3 = m.container([m.shex_chunk([m.insn(RET)], stage=1, major=3)])
proc, fields, _, _ = run("glsl", sm3)
check("shader model 3 refused", fields.get("ok") == "0", fields.get("error", ""))

# A cb read with no reflection data behind it must not invent a uniform.
noreflect = m.container([
    m.signature_chunk([{"name": "TEXCOORD", "index": 0, "register": 0}], b"ISGN"),
    m.signature_chunk([{"name": "SV_Target", "index": 0, "register": 0, "rw_mask": 0}], b"OSGN"),
    m.shex_chunk([m.insn(MOV, m.dest(OUTPUT, 0), m.src_cb(0, 0)) + m.insn(RET)], stage=0),
])
proc, fields, _, _ = run("glsl", noreflect)
check("undeclared cbuffer refused", fields.get("ok") == "0", fields.get("error", ""))
check("undeclared cbuffer reason", "b0" in fields.get("error", ""), fields.get("error", ""))

# ---------------------------------------------------------------------------
# Everything the first version of this translator refused
# ---------------------------------------------------------------------------

DCL_SAMPLER_C = 90
SAMPLE_C, SAMPLE_C_LZ, GATHER4, LD_MS, RESINFO_OP = 70, 71, 109, 46, 61
BFI, UBFE, IBFE, BFREV, COUNTBITS = 140, 138, 139, 141, 134
FIRSTBIT_HI, FIRSTBIT_LO, F32TOF16, F16TOF32 = 135, 136, 130, 131
UADDC, SWAPC_OP, IMUL_OP, LABEL_OP, CALL_OP = 132, 142, 38, 44, 4
DCL_THREAD_GROUP, DCL_UAV_RAW, STORE_RAW, LD_RAW = 155, 157, 166, 165
DCL_TGSM_RAW, SYNC, IMM_ATOMIC_IADD = 159, 190, 180
EMIT, CUT, DCL_GS_INPUT_PRIMITIVE, DCL_GS_OUTPUT_TOPOLOGY, DCL_MAXOUT = 19, 9, 93, 92, 94

# A compile-time texel offset changes which texel is read. The first version of
# this decoder parsed the extended token and threw it away, which is a silent
# wrong answer -- the worst kind. It now becomes GLSL's offset argument.
body = m.insn(SAMPLE, m.dest(OUTPUT, 0), m.src(INPUT, 0), m.src(RESOURCE, 0),
              m.src(SAMPLER_T, 0))
offset_token = 1 | ((1 & 0xF) << 9) | ((-2 & 0xF) << 13)
body = [body[0] | 0x80000000] + [offset_token] + body[1:]
body[0] = (body[0] & ~(0x7F << 24)) | ((len(body)) << 24)
proc, fields, _, source = run("glsl", pixel_shader(body, temps=0))
check("sample offset survives", "textureOffset(_MainTex, vs_TEXCOORD0.xy, ivec2(1, -2))" in source,
      source + fields.get("error", ""))

# nointerpolation on an input is not cosmetic: an integer varying interpolated
# linearly arrives as garbage, which is why D3D marks those constant.
body = m.insn(MOV, m.dest(OUTPUT, 0), m.src(INPUT, 0))
flat_shader = pixel_shader(body, temps=0)
proc, fields, _, source = run("glsl", flat_shader)
check("smooth varying has no qualifier", "\nin vec4 vs_TEXCOORD0;" in source, source)


def pixel_with_dcl_ps_mode(mode):
    """Rebuilds the fragment fixture with a chosen interpolation mode."""
    isgn = m.signature_chunk([
        {"name": "TEXCOORD", "index": 0, "register": 0, "mask": 0x3, "rw_mask": 0x3}], b"ISGN")
    osgn = m.signature_chunk([
        {"name": "SV_Target", "index": 0, "register": 0, "rw_mask": 0}], b"OSGN")
    rdef = globals_cbuffer([variable("_Color", 0, 16, VECTOR4)], 16)
    code = m.insn(DCL_INPUT_PS, m.dest(INPUT, 0, 0x3), controls=mode)
    code += m.insn(DCL_OUTPUT, m.dest(OUTPUT, 0))
    code += m.insn(MOV, m.dest(OUTPUT, 0), m.src(INPUT, 0))
    code += m.insn(RET)
    return m.container([rdef, isgn, osgn, m.shex_chunk([code], stage=0)])


proc, fields, _, source = run("glsl", pixel_with_dcl_ps_mode(1))
check("nointerpolation becomes flat", "flat in vec4 vs_TEXCOORD0;" in source, source)
proc, fields, _, source = run("glsl", pixel_with_dcl_ps_mode(3))
check("centroid survives", "centroid in vec4 vs_TEXCOORD0;" in source, source)

# Depth comparison needs a shadow sampler, and the comparison value folds into
# the coordinate.
body = m.insn(SAMPLE_C_LZ, m.dest(OUTPUT, 0), m.src(INPUT, 0), m.src(RESOURCE, 0),
              m.src(SAMPLER_T, 0), m.src_cb(0, 0, (X, X, X, X)))
proc, fields, _, source = run("glsl", pixel_shader(body, textures=(("_ShadowMap", 4),), temps=0))
check("shadow sampler declared", "uniform highp sampler2DShadow _ShadowMap;" in source,
      source + fields.get("error", ""))
check("comparison folds into the coordinate",
      "texture(_ShadowMap, vec3(vs_TEXCOORD0.xy, _Color.x))" in source, source)

# gather4 is GLSL ES 3.10, so the version escalates on its own.
body = m.insn(GATHER4, m.dest(OUTPUT, 0), m.src(INPUT, 0), m.src(RESOURCE, 0, (Y, Y, Y, Y)),
              m.src(SAMPLER_T, 0))
proc, fields, _, source = run("glsl", pixel_shader(body, temps=0))
check("gather4 translates", fields.get("ok") == "1", fields.get("error", ""))
check("gather4 escalates the version", source.startswith("#version 310 es"), source[:20])
check("gather4 picks its channel", "textureGather(_MainTex, vs_TEXCOORD0.xy, 1)" in source, source)

# Bit manipulation.
body = m.insn(COUNTBITS, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("countbits", "uintBitsToFloat(uint(bitCount(floatBitsToUint(r0.x))))" in source,
      source + fields.get("error", ""))

body = m.insn(BFREV, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("bfrev", "bitfieldReverse(floatBitsToUint(r0.x))" in source, source)

body = m.insn(FIRSTBIT_HI, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("firstbit_hi counts from the high end", "(31 - findMSB(" in source, source)

body = m.insn(FIRSTBIT_LO, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("firstbit_lo is findLSB", "findLSB(" in source, source)

body = m.insn(UBFE, m.dest(OUTPUT, 0, 0x1), m.imm_int(8), m.imm_int(4), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("ubfe masks its width and offset to five bits",
      "bitfieldExtract(floatBitsToUint(r0.x), (4) & 31, (8) & 31)" in source,
      source + fields.get("error", ""))

body = m.insn(BFI, m.dest(OUTPUT, 0, 0x1), m.imm_int(8), m.imm_int(4), m.src(TEMP, 0),
              m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("bfi", "bitfieldInsert(" in source, source + fields.get("error", ""))

body = m.insn(F32TOF16, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("f32tof16 keeps only the low half", "packHalf2x16(vec2(r0.x, 0.0)) & 0xffffu" in source,
      source + fields.get("error", ""))

body = m.insn(F16TOF32, m.dest(OUTPUT, 0, 0x1), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("f16tof32", "unpackHalf2x16(floatBitsToUint(r0.x) & 0xffffu).x" in source, source)

body = m.insn(UADDC, m.dest(TEMP, 0, 0x1), m.dest(TEMP, 0, 0x2), m.src(TEMP, 0), m.src(TEMP, 0))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("uaddc uses uaddCarry", "uaddCarry(" in source, source + fields.get("error", ""))
check("uaddc escalates the version", source.startswith("#version 310 es"), source[:20])

body = m.insn(SWAPC_OP, m.dest(TEMP, 0, 0x1), m.dest(TEMP, 0, 0x2), m.src(TEMP, 0),
              m.src(TEMP, 0), m.src(TEMP, 0))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("swapc reads both sources before writing either", source.count("swapA") >= 2,
      source + fields.get("error", ""))

# The high half of a 32x32 multiply.
body = m.insn(IMUL_OP, m.dest(TEMP, 0, 0x1), m.src_null(), m.src(TEMP, 0), m.src(TEMP, 0))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
proc, fields, _, source = run("glsl", pixel_shader(body))
check("imul high half uses imulExtended", "imulExtended(" in source,
      source + fields.get("error", ""))

# Subroutines: everything before the first label is main, and each label is a
# function of its own. Emitting the stream as one block would run every
# subroutine inline, in order.
body = m.insn(CALL_OP, m.src(m.OPERAND_LABEL, 3))
body += m.insn(MOV, m.dest(OUTPUT, 0), m.src(TEMP, 0))
body += m.insn(RET)
body += m.insn(LABEL_OP, m.src(m.OPERAND_LABEL, 3))
body += m.insn(MOV, m.dest(TEMP, 0), m.src_cb(0, 0))
body += m.insn(RET)
proc, fields, _, source = run("glsl", pixel_shader(body))
check("subroutine becomes a function", "void subroutine3() {" in source,
      source + fields.get("error", ""))
check("subroutine is forward declared", "void subroutine3();" in source, source)
check("call becomes a call", "subroutine3();" in source, source)
check("registers are file scope when subroutines exist",
      source.index("vec4 r0;") < source.index("void subroutine3() {"), source)

# ---------------------------------------------------------------------------
# Compute and geometry
# ---------------------------------------------------------------------------

def compute_shader(body, bindings=(), tgsm=None):
    rdef = m.rdef_chunk(constant_buffers=[], bindings=list(bindings))
    code = m.insn(DCL_THREAD_GROUP, extra=[64, 1, 1])
    if tgsm is not None:
        code += m.insn(DCL_TGSM_RAW, m.dest(m.OPERAND_THREAD_GROUP_SHARED_MEMORY, 0),
                       extra=[tgsm])
    code += m.insn(DCL_TEMPS, extra=[1])
    code += body
    code += m.insn(RET)
    return m.container([rdef, m.shex_chunk([code], stage=5)])


UAV = m.OPERAND_UNORDERED_ACCESS_VIEW
raw_uav = ({"name": "_Result", "type": 8, "dimension": 0, "bind_point": 0},)
body = m.insn(MOV, m.dest(TEMP, 0), m.src(m.OPERAND_INPUT_THREAD_ID, None))
body += m.insn(STORE_RAW, m.dest(UAV, 0, 0x1), m.src(TEMP, 0, (X, X, X, X)),
               m.src(TEMP, 0, (X, X, X, X)))
proc, fields, _, source = run("glsl", compute_shader(body, raw_uav))
check("compute translates", fields.get("ok") == "1", fields.get("error", ""))
check("work group size declared", "layout(local_size_x = 64, local_size_y = 1, "
      "local_size_z = 1) in;" in source, source)
check("storage buffer declared", "buffer _Result_block { uint _Result[]; };" in source, source)
check("thread id aliased once",
      "vec4 vThreadID = uintBitsToFloat(uvec4(gl_GlobalInvocationID, 0u));" in source, source)
check("compute escalates the version", source.startswith("#version 310 es"), source[:20])

body = m.insn(SYNC, controls=(1 << 4))
body += m.insn(LD_RAW, m.dest(TEMP, 0, 0x1), m.imm_int(0), m.src(m.OPERAND_RESOURCE, 0))
proc, fields, _, source = run("glsl", compute_shader(
    body, ({"name": "_Source", "type": 7, "dimension": 0, "bind_point": 0},)))
check("sync becomes barrier", "barrier();" in source, source + fields.get("error", ""))
check("read-only buffer is readonly", "readonly buffer _Source_block" in source, source)

body = m.insn(IMM_ATOMIC_IADD, m.dest(TEMP, 0, 0x1), m.dest(UAV, 0, 0x1), m.imm_int(0),
              m.imm_int(1))
proc, fields, _, source = run("glsl", compute_shader(body, raw_uav))
check("imm_atomic_iadd returns the old value", "atomicAdd(_Result[" in source,
      source + fields.get("error", ""))


def geometry_shader():
    isgn = m.signature_chunk([{"name": "TEXCOORD", "index": 0, "register": 0}], b"ISGN")
    osgn = m.signature_chunk([{"name": "COLOR", "index": 0, "register": 0}], b"OSGN")
    code = m.insn(DCL_GS_INPUT_PRIMITIVE, controls=3)
    code += m.insn(DCL_MAXOUT, extra=[3])
    code += m.insn(DCL_GS_OUTPUT_TOPOLOGY, controls=5)
    code += m.insn(DCL_TEMPS, extra=[1])
    vertex_in = m._operand(m.OPERAND_INPUT, [1, 0], swizzle=(X, Y, Z, W))
    code += m.insn(MOV, m.dest(m.OPERAND_OUTPUT, 0), vertex_in)
    code += m.insn(EMIT)
    code += m.insn(CUT)
    code += m.insn(RET)
    return m.container([isgn, osgn, m.shex_chunk([code], stage=2)])


proc, fields, _, source = run("glsl", geometry_shader())
check("geometry translates", fields.get("ok") == "1", fields.get("error", ""))
check("geometry vertex count survives", "max_vertices = 3" in source, source)
check("geometry escalates to 3.20", source.startswith("#version 320 es"), source[:20])
check("geometry input layout", "layout(triangles) in;" in source, source)
check("geometry output layout", "layout(triangle_strip, max_vertices = 3) out;" in source, source)
check("geometry inputs are arrays", "in vec4 vs_TEXCOORD0[];" in source, source)
check("geometry reads per vertex", "vs_TEXCOORD0[1]" in source, source)
check("emit", "EmitVertex();" in source, source)
check("geometry writes its own varying", "out vec4 vs_COLOR0;" in source, source)
check("cut", "EndPrimitive();" in source, source)


def geometry_passthrough():
    """A geometry shader that passes a semantic straight through."""
    isgn = m.signature_chunk([{"name": "TEXCOORD", "index": 0, "register": 0}], b"ISGN")
    osgn = m.signature_chunk([{"name": "TEXCOORD", "index": 0, "register": 0}], b"OSGN")
    code = m.insn(DCL_GS_INPUT_PRIMITIVE, controls=3)
    code += m.insn(DCL_MAXOUT, extra=[3])
    code += m.insn(DCL_GS_OUTPUT_TOPOLOGY, controls=5)
    code += m.insn(MOV, m.dest(m.OPERAND_OUTPUT, 0),
                   m._operand(m.OPERAND_INPUT, [1, 0], swizzle=(X, Y, Z, W)))
    code += m.insn(EMIT)
    code += m.insn(RET)
    return m.container([isgn, osgn, m.shex_chunk([code], stage=2)])


proc, fields, _, source = run("glsl", geometry_passthrough())
check("a pass-through geometry varying is refused, not redeclared",
      fields.get("ok") == "0", source)
check("pass-through refusal explains the name clash",
      "both its input and its output" in fields.get("error", ""), fields.get("error", ""))

# ---------------------------------------------------------------------------
# Unity's program header in front of the container
# ---------------------------------------------------------------------------
#
# This is what made the translator reject every shader in every real bundle:
# a D3D11 sub-program is not a bare DXBC container, Unity writes its own
# binding header first. The container has to be found, not assumed to be at
# offset zero.

prefixed = dx_prefixed = m.unity_program([
    globals_cbuffer([variable("_Color", 0, 16, VECTOR4)], 16),
    m.signature_chunk([{"name": "TEXCOORD", "index": 0, "register": 0}], b"ISGN"),
    m.signature_chunk([{"name": "SV_Target", "index": 0, "register": 0, "rw_mask": 0}], b"OSGN"),
    m.shex_chunk([m.insn(MOV, m.dest(OUTPUT, 0), m.src_cb(0, 0)) + m.insn(RET)], stage=0),
])
proc, fields, _, source = run("glsl", prefixed)
check("a container behind Unity's header still translates", fields.get("ok") == "1",
      fields.get("error", ""))
check("the translated body is the real one", "SV_Target0.xyzw = _Color.xyzw;" in source, source)

proc, fields, _, _ = run("parse", prefixed)
check("the container offset is reported", fields.get("containerOffset") == "24",
      fields.get("containerOffset", "<missing>"))

# A longer prefix still resolves, and one past the scan window does not -- an
# unbounded search would eventually match four bytes of bytecode.
proc, fields, _, _ = run("glsl", m.unity_program([
    m.shex_chunk([m.insn(RET)], stage=0)], prefix=b"\x00" * 512))
check("a long prefix is still found", fields.get("ok") == "1", fields.get("error", ""))

proc, fields, _, _ = run("glsl", m.unity_program([
    m.shex_chunk([m.insn(RET)], stage=0)], prefix=b"\x00" * 4096))
check("a prefix past the scan window is refused", fields.get("ok") == "0", "")

# The refusal has to say what the bytes were, or every shader in every map
# reports the same sentence and there is nothing to work from.
proc, fields, _, _ = run("glsl", b"\x01\x02\x03\x04" * 8)
check("a non-container names its byte count", "32 program byte(s)" in fields.get("error", ""),
      fields.get("error", ""))
check("a non-container dumps its prefix", "01 02 03 04" in fields.get("error", ""),
      fields.get("error", ""))

# ---------------------------------------------------------------------------
# Malformed input
# ---------------------------------------------------------------------------

check("empty file", run("parse", b"")[1].get("ok") == "0")
check("short file", run("parse", b"DXBC")[1].get("ok") == "0")
check("wrong magic", run("parse", b"XXXX" + vertex[4:])[1].get("ok") == "0")

truncation_failures = 0
for length in range(0, len(vertex)):
    proc, fields, _, _ = run("parse", vertex[:length])
    if proc.returncode != 0 or fields.get("ok") != "0":
        truncation_failures += 1
check("every truncation is rejected cleanly", truncation_failures == 0,
      "%d prefixes misbehaved" % truncation_failures)

random.seed(20260827)
corruption_failures = 0
sources = [vertex, pixel_shader(m.insn(SAMPLE, m.dest(OUTPUT, 0), m.src(INPUT, 0),
                                       m.src(RESOURCE, 0), m.src(SAMPLER_T, 0)), temps=0)]
for trial in range(400):
    data = bytearray(random.choice(sources))
    for _ in range(random.randint(1, 6)):
        data[random.randrange(len(data))] = random.randrange(256)
    for mode in ("parse", "disasm", "glsl"):
        proc, _, _, _ = run(mode, bytes(data))
        if proc.returncode != 0:
            corruption_failures += 1
            if corruption_failures <= 3:
                print("   corrupted %s trial %d exited %d: %s" %
                      (mode, trial, proc.returncode,
                       proc.stderr.decode("utf-8", errors="replace")[:400]))
check("corrupted containers never crash", corruption_failures == 0,
      "%d runs crashed" % corruption_failures)

print("\n%d passed, %d failed" % (passed, failed))
sys.exit(1 if failed else 0)
