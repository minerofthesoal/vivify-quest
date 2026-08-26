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
  // Re-encode mode: --reencode <serialized-file>
  //
  // Decodes every shader's programs, encodes them straight back, decodes the
  // result, and reports whether the two program lists match. That round trip is
  // the property a write-back path lives or dies on: the blob bytes cannot
  // match Unity's (a different LZ4 encoder wrote them) but what comes back out
  // of it must.
  if (std::string(argv[1]) == "--reencode") {
    if (argc < 3) return 2;
    std::ifstream in(argv[2], std::ios::binary);
    if (!in) return 2;
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    auto report = SerializedFileParse::InspectSerializedFile(bytes.data(), bytes.size());
    int shaders = 0;
    int matched = 0;
    int mismatched = 0;
    for (auto const& shader : report.shaders) {
      auto first = SerializedFileParse::DecodeShaderPrograms(bytes.data(), bytes.size(), shader);
      if (first.programs.empty()) continue;
      shaders++;

      auto store = SerializedFileParse::EncodeShaderPrograms(shader.platforms, first.programs);
      if (!store.ok) {
        std::printf("encodeMessage=%s\n", store.message.c_str());
        mismatched++;
        continue;
      }

      // Point a shader at the freshly built store and read it back.
      SerializedFileParse::ShaderObject rebuilt = shader;
      rebuilt.offsets = store.offsets;
      rebuilt.compressedLengths = store.compressedLengths;
      rebuilt.decompressedLengths = store.decompressedLengths;
      rebuilt.blobPresent = true;
      rebuilt.blobFileOffset = 0;
      rebuilt.blobSize = store.blob.size();
      auto second = SerializedFileParse::DecodeShaderPrograms(store.blob.data(), store.blob.size(),
                                                              rebuilt);

      bool same = second.ok && second.programs.size() == first.programs.size();
      for (size_t i = 0; same && i < first.programs.size(); i++) {
        auto const& a = first.programs[i];
        auto const& b = second.programs[i];
        same = a.platform == b.platform && a.groupIndex == b.groupIndex &&
               a.blobIndex == b.blobIndex && a.programIndex == b.programIndex &&
               a.blobVersion == b.blobVersion && a.programType == b.programType &&
               a.entrySize == b.entrySize && a.stats == b.stats && a.keywords == b.keywords &&
               a.localKeywords == b.localKeywords && a.code == b.code && a.trailing == b.trailing;
      }
      if (same) matched++; else mismatched++;
    }
    std::printf("reencodeShaders=%d\n", shaders);
    std::printf("reencodeMatched=%d\n", matched);
    std::printf("reencodeMismatched=%d\n", mismatched);
    return mismatched == 0 ? 0 : 1;
  }

  // Compression mode: --lz4c <plain-file> <out-file>. The output is a raw LZ4
  // block, which run_tests.py hands to the reference lz4 library -- the only
  // way to know this agrees with what Unity's own decompressor will do, rather
  // than only with the decoder in this same file.
  if (std::string(argv[1]) == "--lz4c") {
    if (argc < 4) return 2;
    std::ifstream plainStream(argv[2], std::ios::binary);
    if (!plainStream) return 2;
    std::vector<uint8_t> plain((std::istreambuf_iterator<char>(plainStream)),
                               std::istreambuf_iterator<char>());
    std::vector<uint8_t> packed(SerializedFileParse::Lz4CompressBound(plain.size()));
    size_t const written =
        SerializedFileParse::Lz4CompressBlock(plain.data(), plain.size(), packed.data(), packed.size());
    std::printf("lz4cWritten=%zu\n", written);
    std::printf("lz4cInput=%zu\n", plain.size());
    if (written == 0) return 1;
    std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<char const*>(packed.data()), static_cast<std::streamsize>(written));
    return out.good() ? 0 : 2;
  }

  // Rewrite mode: --rewrite <file> <out> [pathID=hexbytes ...]
  //
  // Prints what the rewritten file parses back as, so run_tests.py can check
  // that objects survived, moved correctly, and kept their identity.
  bool const rewriting = std::string(argv[1]) == "--rewrite";
  int const fileArg = rewriting ? 2 : 1;
  if (rewriting && argc < 4) return 2;

  std::ifstream stream(argv[fileArg], std::ios::binary);
  if (!stream) {
    std::fprintf(stderr, "cannot open %s\n", argv[fileArg]);
    return 2;
  }
  std::vector<uint8_t> data((std::istreambuf_iterator<char>(stream)),
                            std::istreambuf_iterator<char>());

  if (rewriting) {
    std::vector<SerializedFileParse::ObjectEdit> edits;
    for (int i = 4; i < argc; i++) {
      std::string const spec = argv[i];
      auto const split = spec.find('=');
      if (split == std::string::npos) return 2;
      SerializedFileParse::ObjectEdit edit;
      edit.pathID = std::atoll(spec.substr(0, split).c_str());
      std::string const hex = spec.substr(split + 1);
      for (size_t at = 0; at + 1 < hex.size(); at += 2) {
        edit.body.push_back(static_cast<uint8_t>(std::stoul(hex.substr(at, 2), nullptr, 16)));
      }
      edits.push_back(std::move(edit));
    }

    auto rewritten = SerializedFileParse::RewriteSerializedFile(data.data(), data.size(), edits);
    std::printf("rewriteOk=%d\n", rewritten.ok ? 1 : 0);
    if (!rewritten.message.empty()) std::printf("rewriteMessage=%s\n", rewritten.message.c_str());
    if (!rewritten.ok) return 1;
    std::printf("rewriteSize=%zu\n", rewritten.data.size());
    std::printf("identical=%d\n", rewritten.data == data ? 1 : 0);
    std::ofstream out(argv[3], std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<char const*>(rewritten.data.data()),
              static_cast<std::streamsize>(rewritten.data.size()));
    if (!out.good()) return 2;
    data = std::move(rewritten.data);
  }

  auto report = SerializedFileParse::InspectSerializedFile(data.data(), data.size());
  std::printf("isSerializedFile=%d\n", report.isSerializedFile ? 1 : 0);
  std::printf("parsed=%d\n", report.parsed ? 1 : 0);
  std::printf("typeTree=%d\n", report.typeTreePresent ? 1 : 0);
  std::printf("unity=%s\n", report.unityVersion.c_str());
  std::printf("objects=%d\n", report.objectCount);
  std::printf("shaderObjects=%d\n", report.shaderObjectCount);
  for (auto const& shader : report.shaders) {
    std::printf("pathID=%lld\n", static_cast<long long>(shader.pathID));
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
