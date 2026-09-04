import os, sys, subprocess, itertools, struct
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkbundle import build, read_converted, target_of

HERE = os.path.dirname(os.path.abspath(__file__))
CONV = os.environ.get("VIVIFY_CONV", "/tmp/conv")
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

print(f"\n{cases - fails}/{cases} passed")
sys.exit(1 if fails else 0)
