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
    for line in proc.stdout.splitlines():
        if line.startswith("shader="):
            name, _, platforms = line[len("shader="):].partition(" platforms=")
            shaders.append((name, [int(x) for x in platforms.split(",") if x]))
        elif "=" in line:
            key, _, value = line.partition("=")
            fields[key] = value
    return proc, fields, shaders


def check(name, condition, detail=""):
    global passed, failed
    if condition:
        print(f"ok   {name}")
        passed += 1
    else:
        print(f"FAIL {name} {detail}")
        failed += 1


# --- what a PC-built Vivify bundle looks like -------------------------------
_, f, s = run(mkshader.serialized_file_with_shaders([("Custom/Raymarch", [mkshader.D3D11])]))
check("windows shader reports Direct3D 11 only", s == [("Custom/Raymarch", [4])], s)
check("windows shader file parses", f.get("parsed") == "1", f)

# --- an Android-built bundle ------------------------------------------------
_, f, s = run(mkshader.serialized_file_with_shaders(
    [("Custom/Raymarch", [mkshader.GLES3PLUS, mkshader.VULKAN])]))
check("android shader reports GLES3 and Vulkan", s == [("Custom/Raymarch", [9, 18])], s)

# --- several shaders, several platforms each --------------------------------
_, f, s = run(mkshader.serialized_file_with_shaders([
    ("A", [mkshader.D3D11]),
    ("B", [mkshader.D3D11, mkshader.METAL]),
    ("C", [mkshader.GLES3PLUS]),
]))
check("multiple shaders are all read", len(s) == 3, s)
check("per-shader platforms stay separate",
      s == [("A", [4]), ("B", [4, 14]), ("C", [9])], s)

# --- non-shader objects must not be misread ---------------------------------
_, f, s = run(mkshader.serialized_file_with_shaders(
    [("OnlyShader", [mkshader.D3D11])], extra_class_id=43))
check("non-shader object is skipped", f.get("shaderObjects") == "1", f)
check("object count includes the non-shader", f.get("objects") == "2", f)

# --- 64-bit offset variant (SerializedFile version 22) ----------------------
_, f, s = run(mkshader.serialized_file_with_shaders(
    [("Big", [mkshader.D3D11])], sf_version=22))
check("version 22 (64-bit offsets) parses", s == [("Big", [4])], (f, s))

# --- version without ref-type hashes ----------------------------------------
_, f, s = run(mkshader.serialized_file_with_shaders(
    [("Older", [mkshader.D3D11])], sf_version=17))
check("version 17 (24-byte tree nodes) parses", s == [("Older", [4])], (f, s))

# --- type tree stripped -----------------------------------------------------
_, f, s = run(mkshader.serialized_file_with_shaders(
    [("Stripped", [mkshader.D3D11])], enable_type_tree=False))
check("stripped type tree is reported, not guessed",
      f.get("typeTree") == "0" and s == [], (f, s))
check("stripped type tree explains itself", "type tree" in f.get("message", ""), f)

# --- things that are not serialized files -----------------------------------
_, f, _ = run(b"UnityFS\0" + bytes(200))
check("raw non-serialized data is not claimed", f.get("isSerializedFile") == "0", f)
_, f, _ = run(bytes(16))
check("all-zero input is not claimed", f.get("isSerializedFile") == "0", f)
_, f, _ = run(b"")
check("empty input is not claimed", f.get("isSerializedFile") == "0", f)

# --- truncation -------------------------------------------------------------
full = mkshader.serialized_file_with_shaders([("Trunc", [mkshader.D3D11])])
truncation_crashes = 0
for cut in range(8, len(full), 7):
    proc, _, _ = run(full[:cut])
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
    proc, _, _ = run(bytes(data))
    if proc.returncode not in (0, 1):
        fuzz_crashes += 1
check("no corrupted file crashes the parser", fuzz_crashes == 0, f"{fuzz_crashes} bad exits")

print()
print(f"{passed}/{passed + failed} passed")
sys.exit(1 if failed else 0)
