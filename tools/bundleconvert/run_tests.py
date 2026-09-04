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
        os.path.join(root, "src", "VivifyDxbc.cpp"),
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

# --- the whole conversion (conv --shaders) -----------------------------------
#
# A bundle carrying a real DXBC vertex program goes in; a bundle whose shader
# says GLES3 and whose program is GLSL text comes out. The strongest check that
# does not need a second parser written in Python is to run the *output* back
# through the same conversion: a converted bundle already targets Android and
# its shaders already run here, so the second pass must find nothing to do.

sys.path.insert(0, os.path.join(os.path.dirname(HERE), "dxbc"))
import mkdxbc as dx  # noqa: E402
import mkshader  # noqa: E402

DX11_VERTEX_SM50 = 16
DX11_PIXEL_SM50 = 18
GLES3_PLATFORM = 9

MUL, MAD, ADD, RET, DCL_TEMPS, DCL_INPUT, DCL_OUTPUT, DCL_OUTPUT_SIV = 56, 50, 0, 62, 104, 95, 101, 103
SAMPLE, DCL_SAMPLER, DCL_RESOURCE, DCL_INPUT_PS, MSAD = 69, 90, 88, 98, 213


def dxbc_vertex():
    isgn = dx.signature_chunk([{"name": "POSITION", "index": 0, "register": 0}], b"ISGN")
    osgn = dx.signature_chunk(
        [{"name": "SV_POSITION", "index": 0, "register": 0, "sv": 1, "rw_mask": 0}], b"OSGN")
    rdef = dx.rdef_chunk(
        constant_buffers=[{"name": "$Globals", "size": 64, "variables": [
            {"name": "unity_MatrixVP", "offset": 0, "size": 64, "class": 3, "type": 3,
             "rows": 4, "columns": 4, "elements": 0}]}],
        bindings=[{"name": "$Globals", "type": 0, "dimension": 0, "bind_point": 0}])
    code = []
    code += dx.insn(DCL_INPUT, dx.dest(dx.OPERAND_INPUT, 0))
    code += dx.insn(DCL_OUTPUT_SIV, dx.dest(dx.OPERAND_OUTPUT, 0), extra=[1])
    code += dx.insn(DCL_TEMPS, extra=[1])
    code += dx.insn(MUL, dx.dest(dx.OPERAND_TEMP, 0),
                    dx.src(dx.OPERAND_INPUT, 0, (0, 0, 0, 0)), dx.src_cb(0, 0))
    code += dx.insn(ADD, dx.dest(dx.OPERAND_OUTPUT, 0), dx.src(dx.OPERAND_TEMP, 0),
                    dx.src_cb(0, 1))
    code += dx.insn(RET)
    # unity_program, not container: a D3D11 sub-program in a real bundle carries
    # Unity's binding header in front of the DXBC, and a fixture without one
    # tests a shape that never occurs.
    return dx.unity_program([rdef, isgn, osgn, dx.shex_chunk([code], stage=1)])


def dxbc_untranslatable():
    """Same framing, but using an instruction outside the translated subset.

    msad is a sum-of-absolute-differences instruction with no GLSL ES form at
    all, so it stays outside the subset however far the translator grows."""
    isgn = dx.signature_chunk([{"name": "POSITION", "index": 0, "register": 0}], b"ISGN")
    osgn = dx.signature_chunk(
        [{"name": "SV_POSITION", "index": 0, "register": 0, "sv": 1, "rw_mask": 0}], b"OSGN")
    code = []
    code += dx.insn(DCL_TEMPS, extra=[1])
    code += dx.insn(MSAD, dx.dest(dx.OPERAND_TEMP, 0), dx.src(dx.OPERAND_TEMP, 0),
                    dx.src(dx.OPERAND_TEMP, 0), dx.src(dx.OPERAND_TEMP, 0))
    code += dx.insn(RET)
    return dx.unity_program([isgn, osgn, dx.shex_chunk([code], stage=1)])


def shader_bundle(path, shaders, sf_version=21, target=19, textures=None):
    sf = mkshader.serialized_file_with_shaders(shaders, sf_version=sf_version, target=target,
                                               textures=textures)
    return build(path, sf_bytes=[sf], with_resource=False)


def run_shaders(src, dst):
    if os.path.exists(dst):
        os.remove(dst)
    proc = subprocess.run([CONV, "--shaders", src, dst], capture_output=True, text=True)
    fields = {}
    refusals = []
    for line in proc.stdout.splitlines():
        if line.startswith("refusal="):
            refusals.append(line[len("refusal="):])
            continue
        # status and message are prose and hold spaces, so they are read as the
        # whole line; the counters share one line and are split on spaces.
        if line.startswith("status=") or line.startswith("message="):
            key, _, value = line.partition("=")
            fields[key] = value
            continue
        for part in line.split(" "):
            key, _, value = part.partition("=")
            if key and value:
                fields.setdefault(key, value)
    return proc, fields, refusals


def shader_case(name, check, shaders, **kw):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "sh_src.vivify")
    dst = os.path.join(TMP, "sh_dst.vivify")
    shader_bundle(src, shaders, **kw)
    proc, fields, refusals = run_shaders(src, dst)
    problem = check(proc, fields, refusals, dst)
    if problem:
        print(f"FAIL {name}: {problem}\n{proc.stdout}{proc.stderr}")
        fails += 1
    else:
        print(f"ok   {name}")


# One platform group holding one sub-blob holding one program.
one_program = [[mkshader.program_blob([mkshader.sub_program(DX11_VERTEX_SM50, dxbc_vertex())])]]


def expect_translated(proc, fields, refusals, dst):
    if fields.get("status") != "converted":
        return f"status '{fields.get('status')}'"
    if fields.get("translated") != "1" or fields.get("programs") != "1":
        return f"translated={fields.get('translated')} programs={fields.get('programs')}"
    try:
        read_converted(dst)
    except Exception as e:
        return f"converted bundle did not parse: {e}"
    return None


shader_case("a DirectX shader is translated and the bundle still parses",
            expect_translated, [("Custom/Test", [4], one_program)])
shader_case("translation works on SerializedFile v22", expect_translated,
            [("Custom/Test", [4], one_program)], sf_version=22)


def expect_idempotent(proc, fields, refusals, dst):
    problem = expect_translated(proc, fields, refusals, dst)
    if problem:
        return problem
    # Second pass over the converted bundle: nothing left to do.
    again = os.path.join(TMP, "sh_dst2.vivify")
    proc2, fields2, _ = run_shaders(dst, again)
    if fields2.get("status") != "already an Android bundle":
        return f"second pass status '{fields2.get('status')}' ({proc2.stdout})"
    if fields2.get("leftAlone") != "1":
        return f"second pass leftAlone={fields2.get('leftAlone')}"
    if os.path.exists(again):
        return "second pass wrote a file it had nothing to change in"
    return None


shader_case("a converted bundle converts to nothing on a second pass",
            expect_idempotent, [("Custom/Test", [4], one_program)])


def expect_refused(proc, fields, refusals, dst):
    if fields.get("status") != "already an Android bundle":
        return f"status '{fields.get('status')}' (nothing translated, nothing to retarget)"
    if fields.get("refused") != "1":
        return f"refused={fields.get('refused')}"
    if not refusals or "msad" not in refusals[0]:
        return f"refusal did not name the instruction: {refusals}"
    return None


untranslatable = [[mkshader.program_blob(
    [mkshader.sub_program(DX11_VERTEX_SM50, dxbc_untranslatable())])]]
shader_case("an untranslatable shader is refused by name, not half-converted",
            expect_refused, [("Custom/Hard", [4], untranslatable)], target=ANDROID)


def expect_mixed(proc, fields, refusals, dst):
    if fields.get("status") != "converted":
        return f"status '{fields.get('status')}'"
    if fields.get("translated") != "1" or fields.get("refused") != "1":
        return f"translated={fields.get('translated')} refused={fields.get('refused')}"
    try:
        read_converted(dst)
    except Exception as e:
        return f"converted bundle did not parse: {e}"
    return None


shader_case("a bundle with one good and one bad shader keeps both and stays loadable",
            expect_mixed,
            [("Custom/Good", [4], one_program), ("Custom/Bad", [4], untranslatable)])

# --- making a bundle's block-compressed textures decodable on device -------
#
# The mod decodes BC textures to RGBA32 at level load, and it can only do that
# while the texture still has a CPU copy -- which Unity keeps only when
# m_IsReadable is set. Conversion sets it. A texture that never gets the flag
# loses its pixels at load, and decoding what is left produces solid black,
# which is what every converted level looked like from 0.8.0 to 0.9.12.
def texture_case(name, specs, check, **kw):
    global fails, cases
    cases += 1
    src = os.path.join(TMP, "tex_src.vivify")
    dst = os.path.join(TMP, "tex_dst.vivify")
    shader_bundle(src, [("Custom/Test", [4], one_program)], textures=specs, **kw)
    proc, fields, refusals = run_shaders(src, dst)
    problem = check(proc, fields, refusals, dst)
    if problem:
        print(f"FAIL {name}: {problem}\n{proc.stdout}{proc.stderr}")
        fails += 1
    else:
        print(f"ok   {name}")


def expect_textures(seen, readable, streamed=0):
    def check(proc, fields, refusals, dst):
        if fields.get("status") != "converted":
            return f"status '{fields.get('status')}'"
        got = (fields.get("texSeen"), fields.get("texReadable"), fields.get("texStreamed"))
        want = (str(seen), str(readable), str(streamed))
        if got != want:
            return f"seen/readable/streamed {got}, wanted {want}"
        try:
            read_converted(dst)
        except Exception as e:
            return f"converted bundle did not parse: {e}"
        return None
    return check


texture_case("a BC1 texture is marked readable",
             [dict(name="brick", texture_format=10, image_data=bytes(8))],
             expect_textures(1, 1))
texture_case("a BC7 texture is marked readable",
             [dict(name="sky", texture_format=25, image_data=bytes(16))],
             expect_textures(1, 1))
texture_case("an RGBA32 texture is left alone -- the GPU can already sample it",
             [dict(name="plain", texture_format=4, image_data=bytes(64))],
             expect_textures(0, 0))
texture_case("a texture that is already readable is counted but not rewritten",
             [dict(name="brick", texture_format=10, is_readable=True, image_data=bytes(8))],
             expect_textures(1, 0))
texture_case("a streamed texture is marked readable and reported as streamed",
             [dict(name="streamed", texture_format=12, stream_size=4096,
                   stream_path="archive:/x/x.resS")],
             expect_textures(1, 1, streamed=1))
texture_case("several textures are all found, whatever their formats",
             [dict(name="a", texture_format=10, image_data=bytes(8)),
              dict(name="b", texture_format=4, image_data=bytes(64)),
              dict(name="c", texture_format=12, image_data=bytes(16)),
              dict(name="d", texture_format=27, is_readable=True, image_data=bytes(16))],
             expect_textures(3, 2))
texture_case("the texture pass works on SerializedFile v22",
             [dict(name="brick", texture_format=10, image_data=bytes(8))],
             expect_textures(1, 1), sf_version=22)


# The flag has to survive the rebuild that resizes the shader beside it: the
# byte is written into the data before RewriteSerializedFile relays the file
# around a longer shader body, and a relay that dropped it would leave a bundle
# that converts "successfully" and still renders black.
cases += 1
src = os.path.join(TMP, "tex_src.vivify")
dst = os.path.join(TMP, "tex_dst.vivify")
shader_bundle(src, [("Custom/Test", [4], one_program)],
              textures=[dict(name="brick", texture_format=10, image_data=bytes(8))])
proc, fields, refusals = run_shaders(src, dst)
if fields.get("translated") == "1" and fields.get("texReadable") == "1" and b"brick" in open(dst, "rb").read():
    print("ok   a texture keeps its flag through the shader rebuild beside it")
else:
    print("FAIL the texture pass and the shader rewrite do not survive each other:\n" + proc.stdout)
    fails += 1

# Reading the flag back out of the converted bundle, rather than trusting the
# counter that says it was written: converting the output again finds the
# texture and has nothing left to do to it.
cases += 1
again = os.path.join(TMP, "tex_dst2.vivify")
proc2, fields2, _ = run_shaders(dst, again)
if fields2.get("texSeen") == "1" and fields2.get("texReadable") == "0":
    print("ok   the flag is really in the converted bundle, not just in the counter")
else:
    print("FAIL the converted bundle's texture is not readable:\n" + proc2.stdout)
    fails += 1


cases += 1
p = subprocess.run([CONV, "--shaders", bad, os.path.join(TMP, "sh_bad.out")],
                   capture_output=True, text=True)
if p.returncode != 0 and not os.path.exists(os.path.join(TMP, "sh_bad.out")):
    print("ok   shader conversion rejects a non-bundle without leaving output")
else:
    print("FAIL shader conversion accepted a non-bundle:\n" + p.stdout); fails += 1

print(f"\n{cases - fails}/{cases} passed")
sys.exit(1 if fails else 0)
