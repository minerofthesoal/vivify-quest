// Host test for the BC/DXT decoder. Builds real BC1/BC3 blocks by hand from
// known colours and checks the decoded pixels come back as expected.
#include "VivifyTextureDecode.hpp"
#include <cstdio>
#include <cstring>
#include <vector>

using namespace Vivify::TextureDecode;

static int failures = 0;
static void check(bool ok, char const* what) {
  std::printf("%s %s\n", ok ? "ok  " : "FAIL", what);
  if (!ok) failures++;
}

static void put16(std::vector<uint8_t>& v, uint16_t x) { v.push_back(x & 0xFF); v.push_back(x >> 8); }
static uint16_t rgb565(int r, int g, int b) { return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)); }

int main() {
  // --- sizing -------------------------------------------------------------
  check(CompressedSize(Format::DXT1, 4, 4) == 8, "BC1 4x4 is one 8-byte block");
  check(CompressedSize(Format::DXT5, 4, 4) == 16, "BC3 4x4 is one 16-byte block");
  check(CompressedSize(Format::DXT1, 1, 1) == 8, "BC1 1x1 still stores a whole block");
  check(CompressedSizeWithMips(Format::DXT1, 4, 4, 3) == 8 + 8 + 8, "mip chain rounds each level to a block");
  check(FromUnityTextureFormat(10) == Format::DXT1, "Unity DXT1 maps through");
  check(FromUnityTextureFormat(4) == Format::Unsupported, "RGBA32 needs no decoding");

  // --- BC1: a block that is entirely colour0 ------------------------------
  {
    std::vector<uint8_t> block;
    put16(block, rgb565(255, 0, 0));   // colour0 = red
    put16(block, rgb565(0, 0, 255));   // colour1 = blue
    for (int i = 0; i < 4; i++) block.push_back(0x00);  // every texel selects colour0
    std::vector<uint8_t> out;
    bool ok = DecodeToRgba32(Format::DXT1, block.data(), block.size(), 4, 4, 1, out);
    check(ok && out.size() == 4 * 4 * 4, "BC1 solid block decodes to 4x4 RGBA");
    bool allRed = ok;
    for (size_t i = 0; ok && i < out.size(); i += 4) {
      if (out[i] < 240 || out[i + 1] > 16 || out[i + 2] > 16 || out[i + 3] != 255) allRed = false;
    }
    check(allRed, "BC1 colour0-only block is red everywhere");
  }

  // --- BC1: selector 1 picks colour1 --------------------------------------
  {
    std::vector<uint8_t> block;
    put16(block, rgb565(255, 0, 0));
    put16(block, rgb565(0, 0, 255));
    for (int i = 0; i < 4; i++) block.push_back(0x55);  // every texel selects colour1
    std::vector<uint8_t> out;
    bool ok = DecodeToRgba32(Format::DXT1, block.data(), block.size(), 4, 4, 1, out);
    check(ok && out[0] < 16 && out[2] > 240, "BC1 colour1-only block is blue everywhere");
  }

  // --- BC3: colour as above, alpha constant -------------------------------
  {
    std::vector<uint8_t> block;
    block.push_back(0x80);  // alpha0
    block.push_back(0x80);  // alpha1 == alpha0, so all texels take that value
    for (int i = 0; i < 6; i++) block.push_back(0x00);
    put16(block, rgb565(0, 255, 0));
    put16(block, rgb565(0, 255, 0));
    for (int i = 0; i < 4; i++) block.push_back(0x00);
    std::vector<uint8_t> out;
    bool ok = DecodeToRgba32(Format::DXT5, block.data(), block.size(), 4, 4, 1, out);
    check(ok && out[1] > 240 && out[3] == 0x80, "BC3 decodes colour and constant alpha");
  }

  // --- non-multiple-of-four size ------------------------------------------
  {
    std::vector<uint8_t> block;
    put16(block, rgb565(255, 255, 255));
    put16(block, rgb565(0, 0, 0));
    for (int i = 0; i < 4; i++) block.push_back(0x00);
    std::vector<uint8_t> out;
    bool ok = DecodeToRgba32(Format::DXT1, block.data(), block.size(), 3, 2, 1, out);
    check(ok && out.size() == 3 * 2 * 4, "3x2 decodes to exactly 3x2 pixels from one block");
  }

  // --- refusal cases ------------------------------------------------------
  {
    std::vector<uint8_t> tiny(4, 0), out;
    check(!DecodeToRgba32(Format::DXT1, tiny.data(), tiny.size(), 4, 4, 1, out), "short input is refused");
    check(!DecodeToRgba32(Format::Unsupported, tiny.data(), tiny.size(), 4, 4, 1, out), "unsupported format refused");
    check(!DecodeToRgba32(Format::DXT1, tiny.data(), tiny.size(), 0, 4, 1, out), "zero width refused");
  }

  // --- a full mip chain ----------------------------------------------------
  {
    std::vector<uint8_t> data(CompressedSizeWithMips(Format::DXT1, 8, 8, 4), 0);
    std::vector<uint8_t> out;
    bool ok = DecodeToRgba32(Format::DXT1, data.data(), data.size(), 8, 8, 4, out);
    size_t expected = (8*8 + 4*4 + 2*2 + 1*1) * 4;
    check(ok && out.size() == expected, "8x8 with 4 mips decodes every level");
  }

  std::printf("\n%s\n", failures ? "FAILURES" : "all decoder tests passed");
  return failures ? 1 : 0;
}
