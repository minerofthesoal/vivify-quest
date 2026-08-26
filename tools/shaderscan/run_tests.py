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


def reencode(data):
    path = os.path.join(tmpdir, "reencode.sf")
    with open(path, "wb") as handle:
        handle.write(data)
    proc = subprocess.run([BINARY, "--reencode", path], capture_output=True, timeout=120)
    out = {}
    for line in proc.stdout.decode("utf-8", errors="replace").splitlines():
        key, _, value = line.partition("=")
        out[key] = value
    return proc, out


def lz4c(plain):
    src = os.path.join(tmpdir, "plain.bin")
    dst = os.path.join(tmpdir, "packed.lz4")
    with open(src, "wb") as handle:
        handle.write(plain)
    proc = subprocess.run([BINARY, "--lz4c", src, dst], capture_output=True, timeout=60)
    if proc.returncode != 0:
        return None
    with open(dst, "rb") as handle:
        return handle.read()


def rewrite(data, edits=(), sf_version=21):
    """Rewrites a SerializedFile through the C++ writer and reports both what
    the writer said and what the result parses back as."""
    src = os.path.join(tmpdir, "rewrite-in.sf")
    dst = os.path.join(tmpdir, "rewrite-out.sf")
    with open(src, "wb") as handle:
        handle.write(data)
    args = [BINARY, "--rewrite", src, dst]
    args += [f"{path_id}={body.hex()}" for (path_id, body) in edits]
    proc = subprocess.run(args, capture_output=True, timeout=60)
    text = proc.stdout.decode("utf-8", errors="replace")
    fields = {}
    shaders = []
    for line in text.splitlines():
        if line.startswith("shader="):
            name, _, platforms = line[len("shader="):].partition(" platforms=")
            shaders.append((name, [int(x) for x in platforms.split(",") if x]))
        elif line.startswith("program="):
            continue
        elif "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    written = b""
    if os.path.exists(dst):
        with open(dst, "rb") as handle:
            written = handle.read()
    return proc, fields, shaders, written


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


# --- rewriting a serialized file (conversion step 4) ------------------------
#
# The point of the rewriter is that an object's body can change length. The
# object table stores absolute offsets, so everything after a resized object
# moves, and getting that wrong corrupts a bundle that used to load.

plain = mkshader.serialized_file_with_shaders([("Custom/Raymarch", [mkshader.D3D11])])
proc, f, s_, out = rewrite(plain)
check("a rewrite with no edits reproduces the file byte for byte",
      f.get("rewriteOk") == "1" and f.get("identical") == "1" and out == plain, f)

two = mkshader.serialized_file_with_shaders([
    ("A", [mkshader.D3D11]),
    ("B", [mkshader.GLES3PLUS]),
])
proc, f, s_, out = rewrite(two)
check("a multi-object rewrite with no edits is also unchanged",
      f.get("identical") == "1" and out == two, f)

# Growing an object pushes every later object along; the file has to still
# parse, and every shader has to still be found where the table now says.
grown = mkshader.shader_object("Custom/Raymarch", [mkshader.D3D11]) + b"\x00" * 64
proc, f, s_, out = rewrite(plain, [(1, grown)])
check("a grown object still parses back", f.get("parsed") == "1", f)
check("a grown object keeps its shader readable", s_ == [("Custom/Raymarch", [4])], s_)
check("a grown object makes the file bigger",
      int(f.get("rewriteSize", "0")) > len(plain), f)

# Shrinking is the same machinery in reverse, and the strongest check of it is
# that padding an object out and then putting the original body back reproduces
# the file exactly -- every later object has to have moved forward and then back
# to precisely where it started.
_, f, _, grown_file = rewrite(plain, [(1, grown)])
original_body = mkshader.shader_object("Custom/Raymarch", [mkshader.D3D11])
proc, f, s_, back = rewrite(grown_file, [(1, original_body)])
check("shrinking an object back restores the original file exactly",
      back == plain and s_ == [("Custom/Raymarch", [4])], (f, s_))
check("shrinking makes the file smaller again",
      int(f.get("rewriteSize", "0")) < len(grown_file), f)

# With several objects, a resize has to carry the ones after it along.
big_first = mkshader.shader_object("A", [mkshader.D3D11]) + b"\x00" * 128
_, f, _, spread = rewrite(two, [(1, big_first)])
proc, f, s_, out = rewrite(spread, [(1, mkshader.shader_object("A", [mkshader.D3D11]))])
check("a shrunk object still parses back, and later objects follow it",
      out == two and s_ == [("A", [4]), ("B", [9])], (f, s_))

# Object identity has to survive: step 3 will match a rewritten shader to its
# original by pathID, and a rewrite that renumbered them would silently pair the
# wrong programs with the wrong shader.
proc, f, s_, out = rewrite(two, [(2, mkshader.shader_object("B2", [mkshader.VULKAN]))])
check("an edit is applied to the object named by pathID, not by position",
      s_ == [("A", [4]), ("B2", [18])], s_)

# 64-bit offset files take the same path through a different header field.
big = mkshader.serialized_file_with_shaders([("Big", [mkshader.D3D11])], sf_version=22)
proc, f, s_, out = rewrite(big)
check("version 22 rewrites unchanged", f.get("identical") == "1" and out == big, f)
proc, f, s_, out = rewrite(big, [(1, grown)])
check("version 22 rewrites a grown object",
      f.get("parsed") == "1" and s_ == [("Custom/Raymarch", [4])], (f, s_))

# An edit naming an object that is not there must fail rather than be ignored:
# silently dropping a shader rewrite would produce a bundle that looks converted
# and is not.
proc, f, s_, out = rewrite(plain, [(999, b"\x00\x00\x00\x00")])
check("an edit for an unknown pathID is refused",
      f.get("rewriteOk") == "0" and "pathID" in f.get("rewriteMessage", ""), f)

# Rewriting things that are not serialized files at all.
proc, f, s_, out = rewrite(b"UnityFS\0" + bytes(200))
check("rewriting a non-serialized file is refused", f.get("rewriteOk") == "0", f)
proc, f, s_, out = rewrite(b"")
check("rewriting empty input is refused", f.get("rewriteOk") == "0", f)

# Corrupted input into the rewriter, since it now writes files rather than only
# reading them.
rnd3 = random.Random(20260827)
rewrite_crashes = 0
for _ in range(200):
    corrupt = bytearray(two)
    for _ in range(rnd3.randrange(1, 12)):
        corrupt[rnd3.randrange(len(corrupt))] = rnd3.randrange(256)
    proc, _, _, _ = rewrite(bytes(corrupt))
    if proc.returncode not in (0, 1):
        rewrite_crashes += 1
check("no corrupted file crashes the rewriter", rewrite_crashes == 0,
      f"{rewrite_crashes} bad exits")


# --- writing programs back (conversion step 3, the half that is not HLSLcc) ---
#
# A converted shader has to go back into the bundle. The encoder is checked two
# ways: against the reference LZ4 library, so it agrees with the decompressor
# Unity will actually use rather than only with the decoder in the same file;
# and by round-tripping real programs, so nothing is lost on the way out.

try:
    import lz4.block as _lz4
except ImportError:
    _lz4 = None
check("the reference lz4 library is available to cross-check against", _lz4 is not None,
      "pip install lz4")

if _lz4 is not None:
    corpus = [
        GLSL * 40,
        b"x" * 5000,
        bytes(random.Random(11).randrange(256) for _ in range(9000)),
        b"a", b"abcd", b"abcde" * 3, b"\x00" * 100000,
        bytes(random.Random(12).randrange(4) for _ in range(30000)),
    ]
    rnd4 = random.Random(20260828)
    for _ in range(40):
        length = rnd4.randrange(1, 3000)
        alphabet = rnd4.randrange(1, 40)
        corpus.append(bytes(rnd4.randrange(alphabet) for _ in range(length)))

    bad = 0
    packed_total = 0
    plain_total = 0
    for plain in corpus:
        packed = lz4c(plain)
        if packed is None:
            bad += 1
            continue
        plain_total += len(plain)
        packed_total += len(packed)
        try:
            if _lz4.decompress(packed, uncompressed_size=len(plain)) != plain:
                bad += 1
        except Exception:
            bad += 1
    check("every compressed block decompresses under the reference lz4 library",
          bad == 0, f"{bad}/{len(corpus)} failed")
    check("compression actually compresses", packed_total < plain_total,
          f"{packed_total} vs {plain_total}")

    # And the other direction: reference-compressed blocks must decode here,
    # since a real bundle's blobs were written by Unity's encoder, not ours.
    bad = 0
    for plain in corpus[:12]:
        for mode in ("default", "high_compression"):
            block = _lz4.compress(plain, mode=mode, store_size=False)
            _, out = lz4(block, len(plain))
            if out.get("lz4Written") != str(len(plain)):
                bad += 1
    check("reference-compressed blocks decode here", bad == 0, f"{bad} failed")

# Round-tripping real programs through encode and back.
_, out = reencode(mkshader.serialized_file_with_shaders(
    [("Custom/Bitcrush", [mkshader.GLES3PLUS], [[android]])]))
check("re-encoded programs decode back identically",
      out.get("reencodeShaders") == "1" and out.get("reencodeMatched") == "1" and
      out.get("reencodeMismatched") == "0", out)

_, out = reencode(mkshader.serialized_file_with_shaders(
    [("Custom/Both", [mkshader.D3D11, mkshader.GLES3PLUS], [[windows], [android]])]))
check("two platforms survive a re-encode", out.get("reencodeMatched") == "1" and
      out.get("reencodeMismatched") == "0", out)

_, out = reencode(mkshader.serialized_file_with_shaders(
    [("Custom/Wide", [mkshader.GLES3PLUS], [[wide]])]))
check("12-byte table entries survive a re-encode", out.get("reencodeMatched") == "1" and
      out.get("reencodeMismatched") == "0", out)

_, out = reencode(mkshader.serialized_file_with_shaders(
    [("Custom/Older", [mkshader.GLES3PLUS], [[older]])]))
check("the 2018 format's two keyword tables survive a re-encode",
      out.get("reencodeMatched") == "1" and out.get("reencodeMismatched") == "0", out)

many = mkshader.program_blob([
    mkshader.sub_program(mkshader.GLES3, GLSL, keywords=["DIRECTIONAL", "FOG_EXP2", "LIGHTPROBE_SH"]),
    mkshader.sub_program(mkshader.GLES3, b"#version 300 es\nvoid main(){}"),
    mkshader.sub_program(mkshader.GLES3, GLSL * 8),
])
_, out = reencode(mkshader.serialized_file_with_shaders(
    [("Custom/Many", [mkshader.GLES3PLUS], [[many, android]])]))
check("several sub-blobs and keywords survive a re-encode",
      out.get("reencodeMatched") == "1" and out.get("reencodeMismatched") == "0", out)

print()
print(f"{passed}/{passed + failed} passed")
sys.exit(1 if failed else 0)
