// Host test driver for the SerializedFile shader scanner.
//
// Reads a raw SerializedFile from argv[1] and prints what the parser found, one
// field per line, so run_tests.py can assert on it without linking Python to
// C++. Exit code 0 means the file parsed; 1 means it did not.
#include "VivifySerializedFile.hpp"

#include <cstdio>
#include <fstream>
#include <vector>

int main(int argc, char** argv) {
  if (argc < 2) {
    std::fprintf(stderr, "usage: shaderscan <serialized-file>\n");
    return 2;
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
  }
  if (!report.message.empty()) std::printf("message=%s\n", report.message.c_str());
  return report.parsed ? 0 : 1;
}
