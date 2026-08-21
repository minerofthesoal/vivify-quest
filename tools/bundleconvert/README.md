# Host-side tests for the on-device bundle converter

`src/VivifyBundleConvert.cpp` retargets a PC-built Unity AssetBundle to Android
so Quest can load it. It has no Unity or il2cpp dependency, so it can be built
and exercised with a desktop compiler.

```sh
g++ -std=c++20 -O1 -g -fsanitize=address,undefined \
    -I ../../include -o /tmp/conv main.cpp ../../src/VivifyBundleConvert.cpp

python3 run_tests.py    # structural round-trip suite
python3 fuzz.py         # corruption resistance
```

Set `VIVIFY_CONV` to point at the binary if you built it somewhere other than
`/tmp/conv`.

## What is covered

`mkbundle.py` synthesises UnityFS archives from scratch — including its own LZ4
block compressor, and LZMA1 streams produced by Python's `lzma` module — so the
decompressors in the converter are validated against real encoders rather than
against themselves.

`run_tests.py` checks, for each archive variant:

- every `SerializedFile` ends up targeting Android (`BuildTarget` 13);
- **nothing else changes** — payload length, node offsets/sizes/flags/paths, the
  archive version, and every byte of each serialized file outside the 4-byte
  target-platform field;
- raw payload nodes (`.resS`) are left completely untouched;
- the rewritten archive re-parses as a well-formed uncompressed UnityFS bundle.

Variants exercised: all 16 combinations of archive-level and block-level
compression (none / LZMA / LZ4 / LZ4HC); `UnityFS` version 6 and 7 (the latter
with its 16-byte header alignment); blocks-info at the front and at the back of
the file; `SerializedFile` header versions 17, 21 and 22 (the last with the
64-bit large-file header); several source platforms; a bundle that is already
Android-targeted; single- and multi-file archives; and multi-hundred-kilobyte
payloads spanning many storage blocks.

Negative cases: non-bundle input, truncated archives, and the guarantee that no
output file is left behind when conversion fails.

`fuzz.py` mutates valid archives at random byte offsets (600 archives across
all four compression modes) and requires the converter to either succeed or
fail cleanly — never crash, and never trip a sanitizer.
