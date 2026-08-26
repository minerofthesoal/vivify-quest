// Host test driver for the SerializedFile shader scanner.
//
// Reads a raw SerializedFile from argv[1] and prints what the parser found, one
// field per line, so run_tests.py can assert on it without linking Python to
// C++. Exit code 0 means the file parsed; 1 means it did not.
#include "VivifySerializedFile.hpp"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: shaderscan <serialized-file> | shaderscan --lz4 <block> <capacity>\n");
    return 2;
  }
  // Direct access to the block decoder, so its match-copy and length-extension
  // paths can be tested without wrapping them in a whole SerializedFile.
  if (std::string(argv[1]) == "--lz4") {
    if (argc < 4) return 2;
    std::ifstream blockStream(argv[2], std::ios::binary);
    if (!blockStream) return 2;
    std::vector<uint8_t> block((std::istreambuf_iterator<char>(blockStream)),
                               std::istreambuf_iterator<char>());
    size_t const capacity = static_cast<size_t>(std::atoll(argv[3]));
    std::vector<uint8_t> out(capacity);
    size_t const written =
        SerializedFileParse::Lz4DecodeBlock(block.data(), block.size(), out.data(), out.size());
    std::printf("lz4Written=%zu\n", written);
    std::string text(out.begin(), out.begin() + static_cast<long>(written));
    for (char& c : text) {
      if (c < 0x20 || c > 0x7e) c = '.';
    }
    std::printf("lz4Out=%s\n", text.c_str());
    return written > 0 ? 0 : 1;
  }
  std::ifstream stream(argv[1], std::ios::binary);
  if (!stream) {
    std::fprintf(stderr, "cannot open %s\n", argv[1]);
    return 2;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());

  auto report = SerializedFileParse::InspectSerializedFile(data.data(), data.size());
  std::printf("isSerializedFile=%d\n", report.isSerializedFile ? 1 : 0);
  std::printf("parsed=%d\n", report.parsed ? 1 : 0);
  std::printf("typeTree=%d\n", report.typeTreePresent ? 1 : 0);
  std::printf("unity=%s\n", report.unityVersion.c_str());
  std::printf("objects=%d\n", report.objectCount);
  std::printf("shaderObjects=%d\n", report.shaderObjectCount);
  for (auto const& shader : report.shaders) {
    std::printf("shader=%s platforms=", shader.name.c_str());
    for (size_t i = 0; i < shader.platforms.size(); i++) {
      std::printf("%s%d", i ? "," : "", shader.platforms[i]);
    }
    std::printf("\n");
    std::printf("blob=%d\n", shader.blobPresent ? 1 : 0);
    std::printf("blobSize=%zu\n", shader.blobSize);
    std::printf("groups=%zu\n", shader.offsets.size());

    auto decoded = SerializedFileParse::DecodeShaderPrograms(data.data(), data.size(), shader);
    std::printf("decodeOk=%d\n", decoded.ok ? 1 : 0);
    std::printf("programs=%zu\n", decoded.programs.size());
    if (!decoded.message.empty()) std::printf("decodeMessage=%s\n", decoded.message.c_str());
    for (auto const& program : decoded.programs) {
      // The code is printed as text because for a GLES target that is exactly
      // what it is; a binary program would be unreadable here, which is itself
      // the distinction the test is checking.
      std::string code(program.code.begin(), program.code.end());
      for (char& c : code) {
        if (c < 0x20 || c > 0x7e) c = '.';
      }
      // code= is printed last and may contain spaces, so the harness takes
      // everything after it as the value.
      std::printf("program=%d/%d type=%d glsl=%d keywords=%zu code=%s\n", program.platform,
                  program.blobIndex, program.programType,
                  SerializedFileParse::GpuProgramIsGlslSource(program.programType) ? 1 : 0,
                  program.keywords.size(), code.c_str());
    }
  }
  if (!report.message.empty()) std::printf("message=%s\n", report.message.c_str());
  return report.parsed ? 0 : 1;
}
