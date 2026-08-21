import os, sys, random, subprocess
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from mkbundle import build
HERE = os.path.dirname(os.path.abspath(__file__))
CONV = os.environ.get("VIVIFY_CONV", "/tmp/conv"); TMP = os.path.join(HERE, "work")
src = os.path.join(TMP, "fz_src.vivify"); mut = os.path.join(TMP, "fz.vivify")
rnd = random.Random(1234)
crashes = 0
for comp in (0, 1, 2, 3):
    build(src, compression=comp, block_compression=comp, n_files=2, payload=9000)
    base = bytearray(open(src, 'rb').read())
    for trial in range(150):
        b = bytearray(base)
        for _ in range(rnd.randrange(1, 12)):
            b[rnd.randrange(len(b))] = rnd.randrange(256)
        open(mut, 'wb').write(bytes(b))
        p = subprocess.run([CONV, mut, os.path.join(TMP, "fz.out")], capture_output=True)
        # exit 0/1 = clean success/handled failure. Anything else = crash/sanitizer.
        if p.returncode not in (0, 1):
            crashes += 1
            print(f"CRASH comp={comp} trial={trial} rc={p.returncode}")
            print(p.stderr[:3000].decode("utf-8", "replace"))
            open(os.path.join(TMP, f"crash_{comp}_{trial}.bin"), 'wb').write(bytes(b))
            if crashes > 3: sys.exit(1)
print("crashes:", crashes)
sys.exit(1 if crashes else 0)
