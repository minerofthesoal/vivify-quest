#pragma once

// CPU decoding of BC/DXT block-compressed texture data.
//
// Quest's Adreno GPUs support ETC2 and ASTC but not S3TC/BC, so a texture from
// a PC-built AssetBundle -- which is stored as BC1/BC3/BC7 and friends -- cannot
// be sampled at all on device. Decoding the blocks to plain RGBA32 costs memory
// (BC1 is 4 bits per pixel, RGBA32 is 32) but produces something the GPU can
// actually use, which is the difference between an untextured mesh and a
// textured one.
//
// The block decoding itself is done by the vendored bcdec.h; this header is the
// Unity-independent wrapper around it, so it can be unit tested on a host
// compiler.

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace Vivify::TextureDecode {

// The subset of UnityEngine.TextureFormat this can handle. Values match
// UnityEngine.TextureFormat so they can be compared against it directly.
enum class Format : int32_t {
  Unsupported = 0,
  DXT1 = 10,   // BC1
  DXT5 = 12,   // BC3
  BC4 = 26,
  BC5 = 27,
  BC6H = 24,
  BC7 = 25,
};

// Maps a UnityEngine.TextureFormat value to a Format this can decode, or
// Format::Unsupported when there is nothing to do (or nothing we can do).
Format FromUnityTextureFormat(int32_t unityTextureFormat);

std::string_view FormatName(Format format);

// Bytes one mip level of this format occupies at the given size.
size_t CompressedSize(Format format, int width, int height);

// Total bytes for `mipCount` levels starting at width x height.
size_t CompressedSizeWithMips(Format format, int width, int height, int mipCount);

// Decodes every mip level to tightly packed RGBA32, laid out the same way
// Unity's raw texture data is (largest mip first). Returns false and leaves
// `out` untouched if the input is too short or the parameters are implausible.
bool DecodeToRgba32(Format format, uint8_t const* data, size_t dataSize, int width, int height, int mipCount,
                    std::vector<uint8_t>& out);

// True when every byte of a buffer is zero.
//
// This is the shape of the data a texture hands back when its CPU copy is gone:
// the array is the right length, so nothing downstream can tell it apart from
// real pixels, and every block in it decodes to opaque black. Checking for it
// is the difference between "this texture could not be decoded" and a map that
// renders entirely black.
bool IsAllZero(uint8_t const* data, size_t size);

// True when a decoded RGBA32 buffer carries nothing that can be seen: either
// every colour channel is zero, or every pixel is fully transparent.
//
// A texture really can be all black, and refusing that one loses nothing --
// leaving the original in place draws the same thing. Refusing a decode that
// came out blank, on the other hand, is what stops bad input becoming a black
// level.
bool IsBlankRgba32(uint8_t const* rgba, size_t size);

}
