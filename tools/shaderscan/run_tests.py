#!/usr/bin/env python3
"""Tests for the SerializedFile shader scanner.

Builds Unity SerializedFiles carrying real Shader assets (see
tools/bundleconvert/mkshader.py) and checks what the C++ parser extracts from
them. Run under ASan/UBSan; bundles are untrusted input, so the corruption pass
matters as much as the happy path.

  python3 tools/shaderscan/run_tests.py [path-to-shaderscan-binary]
"""
import os
import random
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools", "bundleconvert"))
import mkshader  # noqa: E402

BINARY = sys.argv[1] if len(sys.argv) > 1 else "/tmp/shaderscan"

passed = 0
failed = 0
tmpdir = tempfile.mkdtemp(prefix="shaderscan-")


def run(data):
    path = os.path.join(tmpdir, "case.sf")
    with open(path, "wb") as handle:
        handle.write(data)
    # errors="replace": a corrupted fixture can still put odd bytes in a
    # shader name, and the harness must not die where the parser did not.
    proc = subprocess.run([BINARY, path], capture_output=True, timeout=60)
    proc.stdout = proc.stdout.decode("utf-8", errors="replace")
    fields = {}
    shaders = []
    programs = []
    for line in proc.stdout.splitlines():
        if line.startswith("shader="):
            name, _, platforms = line[len("shader="):].partition(" platforms=")
            shaders.append((name, [int(x) for x in platforms.split(",") if x]))
        elif line.startswith("program="):
            # code= is last and can contain spaces, so it is split off first.
            head, _, code = line.partition(" code=")
            entry = {"code": code}
            for part in head.split(" "):
                key, _, value = part.partition("=")
                entry[key] = value
            programs.append(entry)
        elif "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    return proc, fields, shaders, programs


def run3(data):
    proc, fields, shaders, _ = run(data)
    return proc, fields, shaders


def lz4(block, capacity):
    path = os.path.join(tmpdir, "block.lz4")
    with open(path, "wb") as handle:
        handle.write(block)
    proc = subprocess.run([BINARY, "--lz4", path, str(capacity)],
                          capture_output=True, timeout=60)
    out = {}
    for line in proc.stdout.decode("utf-8", errors="replace").splitlines():
        key, _, value = line.partition("=")
        out[key] = value
    return proc, out


def check(name, condition, detail=""):
    global passed, failed
    if condition:
        print(f"ok   {name}")
        passed += 1
    else:
        print(f"FAIL {name} {detail}")
        failed += 1


# --- what a PC-built Vivify bundle looks like -------------------------------
_, f, s = run3(mkshader.serialized_file_with_shaders([("Custom/Raymarch", [mkshader.D3D11])]))
check("windows shader reports Direct3D 11 only", s == [("Custom/Raymarch", [4])], s)
check("windows shader file parses", f.get("parsed") == "1", f)

# --- an Android-built bundle ------------------------------------------------
_, f, s = run3(mkshader.serialized_file_with_shaders(
    [("Custom/Raymarch", [mkshader.GLES3PLUS, mkshader.VULKAN])]))
check("android shader reports GLES3 and Vulkan", s == [("Custom/Raymarch", [9, 18])], s)

# --- several shaders, several platforms each --------------------------------
_, f, s = run3(mkshader.serialized_file_with_shaders([
    ("A", [mkshader.D3D11]),
    ("B", [mkshader.D3D11, mkshader.METAL]),
    ("C", [mkshader.GLES3PLUS]),
]))
check("multiple shaders are all read", len(s) == 3, s)
check("per-shader platforms stay separate",
      s == [("A", [4]), ("B", [4, 14]), ("C", [9])], s)

# --- non-shader objects must not be misread ---------------------------------
_, f, s = run3(mkshader.serialized_file_with_shaders(
    [("OnlyShader", [mkshader.D3D11])], extra_class_id=43))
check("non-shader object is skipped", f.get("shaderObjects") == "1", f)
check("object count includes the non-shader", f.get("objects") == "2", f)

# --- 64-bit offset variant (SerializedFile version 22) ----------------------
_, f, s = run3(mkshader.serialized_file_with_shaders(
    [("Big", [mkshader.D3D11])], sf_version=22))
check("version 22 (64-bit offsets) parses", s == [("Big", [4])], (f, s))

# --- version without ref-type hashes ----------------------------------------
_, f, s = run3(mkshader.serialized_file_with_shaders(
    [("Older", [mkshader.D3D11])], sf_version=17))
check("version 17 (24-byte tree nodes) parses", s == [("Older", [4])], (f, s))

# --- type tree stripped -----------------------------------------------------
_, f, s = run3(mkshader.serialized_file_with_shaders(
    [("Stripped", [mkshader.D3D11])], enable_type_tree=False))
check("stripped type tree is reported, not guessed",
      f.get("typeTree") == "0" and s == [], (f, s))
check("stripped type tree explains itself", "type tree" in f.get("message", ""), f)

# --- things that are not serialized files -----------------------------------
_, f, _ = run3(b"UnityFS\0" + bytes(200))
check("raw non-serialized data is not claimed", f.get("isSerializedFile") == "0", f)
_, f, _ = run3(bytes(16))
check("all-zero input is not claimed", f.get("isSerializedFile") == "0", f)
_, f, _ = run3(b"")
check("empty input is not claimed", f.get("isSerializedFile") == "0", f)

# --- truncation -------------------------------------------------------------
full = mkshader.serialized_file_with_shaders([("Trunc", [mkshader.D3D11])])
truncation_crashes = 0
for cut in range(8, len(full), 7):
    proc, _, _ = run3(full[:cut])
    if proc.returncode not in (0, 1):
        truncation_crashes += 1
check("no truncation crashes the parser", truncation_crashes == 0,
      f"{truncation_crashes} bad exits")

# --- corruption fuzz --------------------------------------------------------
rnd = random.Random(20260825)
fuzz_crashes = 0
for _ in range(300):
    data = bytearray(full)
    for _ in range(rnd.randrange(1, 12)):
        data[rnd.randrange(len(data))] = rnd.randrange(256)
    proc, _, _ = run3(bytes(data))
    if proc.returncode not in (0, 1):
        fuzz_crashes += 1
check("no corrupted file crashes the parser", fuzz_crashes == 0, f"{fuzz_crashes} bad exits")


# --- the LZ4 block decoder --------------------------------------------------
block, expected = mkshader.lz4_block_with_match()
_, out = lz4(block, 64)
check("lz4 match copy reproduces the back-reference",
      out.get("lz4Out") == expected.decode() and out.get("lz4Written") == str(len(expected)), out)

literal = b"the quick brown fox jumps over the lazy dog, twice over and then some"
_, out = lz4(mkshader.lz4_literals(literal), 256)
check("lz4 literal run with length extension round-trips",
      out.get("lz4Out") == literal.decode(), out)

_, out = lz4(mkshader.lz4_literals(b"exactly"), 3)
check("lz4 refuses to write past the output buffer", out.get("lz4Written") == "0", out)

# A back-reference pointing further back than anything written yet.
_, out = lz4(b"\x00" + (99).to_bytes(2, "little") + b"\x10A", 64)
check("lz4 rejects an offset before the start of the output",
      out.get("lz4Written") == "0", out)

# --- decoding a real program store ------------------------------------------
GLSL = b"#version 300 es\nvoid main(){gl_Position=vec4(0.0);}"
android = mkshader.program_blob([
    mkshader.sub_program(mkshader.GLES3, GLSL, keywords=["DIRECTIONAL"]),
    mkshader.sub_program(mkshader.GLES3, b"#version 300 es\nvoid main(){}"),
])
_, f, s, p = run(mkshader.serialized_file_with_shaders(
    [("Custom/Bitcrush", [mkshader.GLES3PLUS], [[android]])]))
check("shader with a program store still reports its platform",
      s == [("Custom/Bitcrush", [9])], s)
check("compressedBlob is located", f.get("blob") == "1", f)
check("one length-table group per platform", f.get("groups") == "1", f)
check("both sub-programs decode", f.get("decodeOk") == "1" and f.get("programs") == "2", f)
check("program code is the GLSL source Unity stored",
      p and p[0].get("code") == GLSL.decode().replace("\n", "."), p)
check("program is recognised as GLSL source", p and p[0].get("glsl") == "1", p)
check("program keywords are read", p and p[0].get("keywords") == "1", p)
check("program carries its platform", p and p[0].get("program") == "9/0", p)

# --- two platforms, each with its own blob -----------------------------------
windows = mkshader.program_blob([mkshader.sub_program(mkshader.DX11_PIXEL_SM50, b"DXBC\x00\x01")])
_, f, s, p = run(mkshader.serialized_file_with_shaders(
    [("Custom/Both", [mkshader.D3D11, mkshader.GLES3PLUS], [[windows], [android]])]))
check("both platforms decode", f.get("programs") == "3", f)
check("each program keeps the platform of its own blob",
      sorted(x["program"] for x in p) == ["4/0", "9/0", "9/0"], p)
check("DirectX program is not claimed to be GLSL source",
      [x["glsl"] for x in p if x["program"] == "4/0"] == ["0"], p)

# --- 12-byte program table entries -------------------------------------------
wide = mkshader.program_blob([mkshader.sub_program(mkshader.GLES3, GLSL)], entry_size=12)
_, f, s, p = run(mkshader.serialized_file_with_shaders(
    [("Custom/Wide", [mkshader.GLES3PLUS], [[wide]])]))
check("12-byte program table entries are recognised",
      f.get("programs") == "1" and p and p[0].get("code") == GLSL.decode().replace("\n", "."),
      (f, p))

# --- the older sub-program format with a local keyword table -----------------
older = mkshader.program_blob([
    mkshader.sub_program(mkshader.GLES3, GLSL, blob_version=201806140, keywords=["FOG"]),
])
_, f, s, p = run(mkshader.serialized_file_with_shaders(
    [("Custom/Older", [mkshader.GLES3PLUS], [[older]])]))
check("2018-format sub-program with a local keyword table decodes",
      f.get("programs") == "1" and p and p[0].get("code") == GLSL.decode().replace("\n", "."),
      (f, p))

# --- a shader with no program store at all -----------------------------------
_, f, s, p = run(mkshader.serialized_file_with_shaders([("Bare", [mkshader.D3D11])]))
check("a shader with empty length tables decodes to nothing, without failing",
      f.get("programs") == "0" and "could not be read" not in f.get("decodeMessage", ""), f)

# --- corrupted program stores must not be trusted ----------------------------
store_file = mkshader.serialized_file_with_shaders(
    [("Custom/Bitcrush", [mkshader.GLES3PLUS], [[android]])])
store_crashes = 0
for cut in range(8, len(store_file), 5):
    proc, _, _ = run3(store_file[:cut])
    if proc.returncode not in (0, 1):
        store_crashes += 1
check("no truncation of a program store crashes the decoder", store_crashes == 0,
      f"{store_crashes} bad exits")

rnd2 = random.Random(20260826)
store_fuzz = 0
for _ in range(300):
    data = bytearray(store_file)
    for _ in range(rnd2.randrange(1, 12)):
        data[rnd2.randrange(len(data))] = rnd2.randrange(256)
    proc, _, _ = run3(bytes(data))
    if proc.returncode not in (0, 1):
        store_fuzz += 1
check("no corrupted program store crashes the decoder", store_fuzz == 0,
      f"{store_fuzz} bad exits")

print()
print(f"{passed}/{passed + failed} passed")
sys.exit(1 if failed else 0)
