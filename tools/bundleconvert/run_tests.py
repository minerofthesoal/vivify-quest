import os, sys, subprocess, itertools, struct, tempfile
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkbundle import build, read_converted, target_of

HERE = os.path.dirname(os.path.abspath(__file__))
def _build_default_binary() -> str:
    """Compile the converter from the current sources.

    Defaulting to a prebuilt /tmp/conv meant a local run could pass against a
    binary built before the change under test -- which is exactly how a link
    error against VivifySerializedFile.cpp reached CI green-looking. Set
    VIVIFY_CONV to skip this and test a binary you built yourself.
    """
    root = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    out = os.path.join(tempfile.mkdtemp(prefix="vivify-conv-"), "conv")
    cmd = [
        os.environ.get("CXX", "g++"), "-std=c++20", "-O1", "-g",
        "-fsanitize=address,undefined", "-I", os.path.join(root, "include"),
        "-o", out,
        os.path.join(root, "tools", "bundleconvert", "main.cpp"),
        os.path.join(root, "src", "VivifyBundleConvert.cpp"),
        os.path.join(root, "src", "VivifySerializedFile.cpp"),
    ]
    subprocess.run(cmd, check=True)
    return out


CONV = os.environ.get("VIVIFY_CONV") or _build_default_binary()
TMP  = os.path.join(HERE, "work")
os.makedirs(TMP, exist_ok=True)

ANDROID = 13
fails = 0
cases = 0

def case(name, **kw):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "src.vivify")
    dst = os.path.join(TMP, "dst.vivify")
    if os.path.exists(dst): os.remove(dst)
    expect = kw.pop("expect", "converted")
    orig, nodes = build(src, **kw)
    proc = subprocess.run([CONV, src, dst], capture_output=True, text=True)
    out = proc.stdout + proc.stderr
    status = ""
    for line in proc.stdout.splitlines():
        if line.startswith("status="): status = line[len("status="):]
    if status != expect:
        print(f"FAIL {name}: expected status '{expect}', got '{status}'\n{out}")
        fails += 1
        return
    if expect != "converted":
        print(f"ok   {name} ({status})")
        return
    try:
        version, uver, urev, cnodes, cdata = read_converted(dst)
    except Exception as e:
        print(f"FAIL {name}: converted bundle did not parse: {e}\n{out}")
        fails += 1
        return
    problems = []
    if version != kw.get("version", 6): problems.append("archive version changed")
    if [n[3] for n in cnodes] != [n[3] for n in nodes]: problems.append("node paths changed")
    if [(n[0], n[1], n[2]) for n in cnodes] != [(n[0], n[1], n[2]) for n in nodes]:
        problems.append("node offsets/sizes/flags changed")
    if len(cdata) != len(orig): problems.append(f"payload length {len(cdata)} != {len(orig)}")
    else:
        sfv = kw.get("sf_version", 17)
        patched = 0
        for (o, s, f, nm) in nodes:
            if nm.endswith(".resS"):
                if cdata[o:o+s] != orig[o:o+s]: problems.append(f"resource node {nm} was modified")
                continue
            t = target_of(cdata, o, s, sfv)
            if t != ANDROID: problems.append(f"{nm} target={t}, expected Android")
            patched += 1
            # everything except the 4 target bytes must be byte-identical
            head_len = 48 if sfv >= 22 else 20
            p = o + head_len
            e = cdata.index(b"\0", p); p = e + 1
            if cdata[o:p] != orig[o:p] or cdata[p+4:o+s] != orig[p+4:o+s]:
                problems.append(f"{nm} changed outside the target-platform field")
        if patched == 0: problems.append("no serialized files patched")
    if problems:
        print(f"FAIL {name}: " + "; ".join(problems))
        fails += 1
    else:
        print(f"ok   {name}")

# archive-level compression x block-level compression
for comp in (0, 1, 2, 3):
    for bcomp in (0, 1, 2, 3):
        case(f"comp={comp} blockcomp={bcomp}", compression=comp, block_compression=bcomp)

# archive header variants
case("version 7 (16-byte aligned header)", version=7)
case("blocks-info at end of file", info_at_end=True)
case("version 7 + info at end", version=7, info_at_end=True)

# SerializedFile header variants
case("SerializedFile v22 (large-file header)", sf_version=22)
case("SerializedFile v21", sf_version=21)
case("SerializedFile v22 + lzma", sf_version=22, compression=1, block_compression=1)

# platform variants
case("source StandaloneWindows (5)", target=5)
case("source StandaloneOSX (2)", target=2)
case("already Android", target=13, expect="already an Android bundle")

# shape variants
case("single serialized file, no resource", n_files=1, with_resource=False)
case("many files / many blocks", n_files=5, payload=60000, block_size=0x2000)
case("large payload, lzma", n_files=3, payload=300000, compression=1, block_compression=1, block_size=0x20000)
case("large payload, lz4", n_files=3, payload=300000, compression=2, block_compression=2, block_size=0x20000)

# negative cases
bad = os.path.join(TMP, "bad.vivify")
open(bad, "wb").write(b"NotAUnityArchive\0garbage" * 10)
p = subprocess.run([CONV, bad, os.path.join(TMP, "bad.out")], capture_output=True, text=True)
cases += 1
if "not a UnityFS asset bundle" in p.stdout:
    print("ok   rejects non-bundle input")
else:
    print("FAIL rejects non-bundle input:\n" + p.stdout); fails += 1

src = os.path.join(TMP, "trunc_src.vivify")
build(src, compression=2, block_compression=2)
raw = open(src, "rb").read()
open(bad, "wb").write(raw[:len(raw)//2])
p = subprocess.run([CONV, bad, os.path.join(TMP, "bad.out")], capture_output=True, text=True)
cases += 1
if p.returncode != 0 and "status=converted" not in p.stdout:
    print("ok   rejects truncated bundle")
else:
    print("FAIL rejects truncated bundle:\n" + p.stdout); fails += 1

cases += 1
if not os.path.exists(os.path.join(TMP, "bad.out")):
    print("ok   no output file left behind on failure")
else:
    print("FAIL stale output file left behind"); fails += 1

# --- the step-4 rewrite path (conv --repack) ---------------------------------
#
# RepackBundle unpacks a bundle, rebuilds every SerializedFile inside it through
# the rewriter, and repacks it. With no shader edits the payload must come back
# exactly as it went in: a resized object is the whole point of the rewriter,
# and if the no-op case does not round-trip then nothing built on it can be
# trusted with a real bundle.

def repack_case(name, **kw):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "rp_src.vivify")
    dst = os.path.join(TMP, "rp_dst.vivify")
    if os.path.exists(dst):
        os.remove(dst)
    orig, nodes = build(src, **kw)
    proc = subprocess.run([CONV, "--repack", src, dst], capture_output=True, text=True)
    status = ""
    for line in proc.stdout.splitlines():
        if line.startswith("status="):
            status = line[len("status="):]
    if status != "converted":
        print(f"FAIL {name}: repack status '{status}'\n{proc.stdout}{proc.stderr}")
        fails += 1
        return
    try:
        version, uver, urev, cnodes, cdata = read_converted(dst)
    except Exception as e:
        print(f"FAIL {name}: repacked bundle did not parse: {e}")
        fails += 1
        return
    problems = []
    if [n[3] for n in cnodes] != [n[3] for n in nodes]:
        problems.append("node paths changed")
    if [(n[0], n[1], n[2]) for n in cnodes] != [(n[0], n[1], n[2]) for n in nodes]:
        problems.append(f"node offsets/sizes changed: {[(n[0], n[1]) for n in cnodes]} != "
                        f"{[(n[0], n[1]) for n in nodes]}")
    if cdata != orig:
        problems.append(f"payload changed ({len(cdata)} vs {len(orig)} bytes)")
    if problems:
        print(f"FAIL {name}: " + "; ".join(problems))
        fails += 1
    else:
        print(f"ok   {name}")


repack_case("repack round-trips the payload byte for byte")
repack_case("repack round-trips SerializedFile v22", sf_version=22)
repack_case("repack round-trips several files and blocks",
            n_files=5, payload=60000, block_size=0x2000)
repack_case("repack round-trips a bundle with no resource node",
            n_files=1, with_resource=False)
repack_case("repack round-trips an lz4-compressed source",
            compression=2, block_compression=2)
repack_case("repack round-trips an already-Android bundle", target=ANDROID)

cases += 1
p = subprocess.run([CONV, "--repack", bad, os.path.join(TMP, "rp_bad.out")],
                   capture_output=True, text=True)
if p.returncode != 0 and not os.path.exists(os.path.join(TMP, "rp_bad.out")):
    print("ok   repack rejects a non-bundle without leaving output")
else:
    print("FAIL repack accepted a non-bundle:\n" + p.stdout); fails += 1

print(f"\n{cases - fails}/{cases} passed")
sys.exit(1 if fails else 0)
