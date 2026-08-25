# Vendored third-party sources

## `bcdec.h`

Single-header BC1–BC7 (DXT) block decoder from
[iOrange/bcdec](https://github.com/iOrange/bcdec), dual-licensed MIT / public
domain (see `bcdec.LICENSE`).

Vendored rather than pulled through QPM because it is not published on
qpackages.com, it has **no includes of its own**, and it is a single header —
so vendoring costs one file and removes a network dependency entirely.

Used by `src/VivifyTextureDecode.cpp` to decode BC/DXT texture data, which an
Adreno GPU cannot sample: Quest supports ETC2 and ASTC but not S3TC/BC. A
PC-built AssetBundle stores its textures block-compressed in one of those BC
formats, so a converted bundle's textures are unusable until they are decoded
to something the GPU understands.
