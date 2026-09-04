#include "VivifyTextureDecode.hpp"

#include <algorithm>
#include <cstring>

#define BCDEC_IMPLEMENTATION 1
#include "third_party/bcdec.h"

namespace Vivify::TextureDecode {

namespace {

constexpr int kRgbaChannels = 4;

int BlockBytes(Format format) {
  switch (format) {
    case Format::DXT1:
    case Format::BC4:
      return 8;
    case Format::DXT5:
    case Format::BC5:
    case Format::BC6H:
    case Format::BC7:
      return 16;
    case Format::Unsupported:
      break;
  }
  return 0;
}

int BlocksAcross(int size) {
  // Block-compressed mips round up to whole 4x4 blocks, and never go below one.
  return std::max(1, (size + 3) / 4);
}

// Decodes one 4x4 block into `dst` as RGBA32 rows of `pitch` bytes.
void DecodeBlock(Format format, uint8_t const* src, uint8_t* dst, int pitch) {
  switch (format) {
    case Format::DXT1:
      bcdec_bc1(src, dst, pitch);
      return;
    case Format::DXT5:
      bcdec_bc3(src, dst, pitch);
      return;
    case Format::BC7:
      bcdec_bc7(src, dst, pitch);
      return;
    case Format::BC4: {
      // Single channel: expand to grey so it is at least legible as a texture.
      uint8_t red[16];
      bcdec_bc4(src, red, 4);
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          uint8_t* pixel = dst + (y * pitch) + (x * kRgbaChannels);
          uint8_t const value = red[(y * 4) + x];
          pixel[0] = pixel[1] = pixel[2] = value;
          pixel[3] = 255;
        }
      }
      return;
    }
    case Format::BC5: {
      uint8_t rg[32];
      bcdec_bc5(src, rg, 8);
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          uint8_t* pixel = dst + (y * pitch) + (x * kRgbaChannels);
          pixel[0] = rg[(y * 8) + (x * 2)];
          pixel[1] = rg[(y * 8) + (x * 2) + 1];
          pixel[2] = 0;
          pixel[3] = 255;
        }
      }
      return;
    }
    case Format::BC6H: {
      // HDR half-float RGB; tonemap crudely into 8-bit so it is usable.
      float rgb[48];
      bcdec_bc6h_float(src, rgb, 12, 0);
      for (int y = 0; y < 4; y++) {
        for (int x = 0; x < 4; x++) {
          uint8_t* pixel = dst + (y * pitch) + (x * kRgbaChannels);
          for (int c = 0; c < 3; c++) {
            float value = rgb[(y * 12) + (x * 3) + c];
            if (!(value > 0.0f)) value = 0.0f;          // also catches NaN
            value = value / (value + 1.0f);             // Reinhard
            pixel[c] = static_cast<uint8_t>(std::min(255.0f, (value * 255.0f) + 0.5f));
          }
          pixel[3] = 255;
        }
      }
      return;
    }
    case Format::Unsupported:
      return;
  }
}

}  // namespace

Format FromUnityTextureFormat(int32_t unityTextureFormat) {
  switch (unityTextureFormat) {
    case 10: return Format::DXT1;
    case 12: return Format::DXT5;
    case 24: return Format::BC6H;
    case 25: return Format::BC7;
    case 26: return Format::BC4;
    case 27: return Format::BC5;
    default: return Format::Unsupported;
  }
}

std::string_view FormatName(Format format) {
  switch (format) {
    case Format::DXT1: return "DXT1/BC1";
    case Format::DXT5: return "DXT5/BC3";
    case Format::BC4: return "BC4";
    case Format::BC5: return "BC5";
    case Format::BC6H: return "BC6H";
    case Format::BC7: return "BC7";
    case Format::Unsupported: break;
  }
  return "unsupported";
}

size_t CompressedSize(Format format, int width, int height) {
  int const blockBytes = BlockBytes(format);
  if (blockBytes == 0 || width <= 0 || height <= 0) return 0;
  return static_cast<size_t>(BlocksAcross(width)) * static_cast<size_t>(BlocksAcross(height)) *
         static_cast<size_t>(blockBytes);
}

size_t CompressedSizeWithMips(Format format, int width, int height, int mipCount) {
  size_t total = 0;
  int w = width;
  int h = height;
  for (int mip = 0; mip < std::max(1, mipCount); mip++) {
    total += CompressedSize(format, w, h);
    if (w == 1 && h == 1) break;
    w = std::max(1, w / 2);
    h = std::max(1, h / 2);
  }
  return total;
}

bool DecodeToRgba32(Format format, uint8_t const* data, size_t dataSize, int width, int height, int mipCount,
                    std::vector<uint8_t>& out) {
  if (format == Format::Unsupported || data == nullptr) return false;
  if (width <= 0 || height <= 0 || width > (1 << 15) || height > (1 << 15)) return false;

  mipCount = std::max(1, mipCount);
  if (dataSize < CompressedSizeWithMips(format, width, height, mipCount)) return false;

  size_t decodedSize = 0;
  {
    int w = width;
    int h = height;
    for (int mip = 0; mip < mipCount; mip++) {
      decodedSize += static_cast<size_t>(w) * static_cast<size_t>(h) * kRgbaChannels;
      if (w == 1 && h == 1) break;
      w = std::max(1, w / 2);
      h = std::max(1, h / 2);
    }
  }

  std::vector<uint8_t> decoded(decodedSize);
  int const blockBytes = BlockBytes(format);
  size_t srcOffset = 0;
  size_t dstOffset = 0;
  int w = width;
  int h = height;

  for (int mip = 0; mip < mipCount; mip++) {
    int const blocksX = BlocksAcross(w);
    int const blocksY = BlocksAcross(h);
    int const pitch = w * kRgbaChannels;

    // A mip whose size is not a multiple of four still stores whole blocks, so
    // decode each block into scratch and copy back only the pixels that exist.
    uint8_t block[4 * 4 * kRgbaChannels];
    for (int by = 0; by < blocksY; by++) {
      for (int bx = 0; bx < blocksX; bx++) {
        if (srcOffset + static_cast<size_t>(blockBytes) > dataSize) return false;
        DecodeBlock(format, data + srcOffset, block, 4 * kRgbaChannels);
        srcOffset += static_cast<size_t>(blockBytes);

        for (int y = 0; y < 4; y++) {
          int const pixelY = (by * 4) + y;
          if (pixelY >= h) break;
          int const copyWidth = std::min(4, w - (bx * 4));
          if (copyWidth <= 0) break;
          std::memcpy(decoded.data() + dstOffset + (static_cast<size_t>(pixelY) * pitch) +
                          (static_cast<size_t>(bx) * 4 * kRgbaChannels),
                      block + (static_cast<size_t>(y) * 4 * kRgbaChannels),
                      static_cast<size_t>(copyWidth) * kRgbaChannels);
        }
      }
    }

    dstOffset += static_cast<size_t>(w) * static_cast<size_t>(h) * kRgbaChannels;
    if (w == 1 && h == 1) break;
    w = std::max(1, w / 2);
    h = std::max(1, h / 2);
  }

  out = std::move(decoded);
  return true;
}

bool IsAllZero(uint8_t const* data, size_t size) {
  if (data == nullptr || size == 0) return true;
  for (size_t i = 0; i < size; i++) {
    if (data[i] != 0) return false;
  }
  return true;
}

bool IsBlankRgba32(uint8_t const* rgba, size_t size) {
  if (rgba == nullptr || size < kRgbaChannels) return true;
  bool anyColour = false;
  bool anyOpacity = false;
  for (size_t i = 0; i + kRgbaChannels <= size; i += kRgbaChannels) {
    if (rgba[i] != 0 || rgba[i + 1] != 0 || rgba[i + 2] != 0) anyColour = true;
    if (rgba[i + 3] != 0) anyOpacity = true;
    if (anyColour && anyOpacity) return false;
  }
  return !(anyColour && anyOpacity);
}

}
