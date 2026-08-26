#include "VivifyBundleConvert.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Vivify::BundleConvert;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: conv [--repack] <src> <dst>\n");
    return 2;
  }
  // --repack runs the bundle through the step-4 rewrite path with no shader
  // edits, which must leave a bundle that reads back the same.
  bool const repack = std::string(argv[1]) == "--repack";
  if (repack && argc < 4) { std::fprintf(stderr, "usage: conv --repack <src> <dst>\n"); return 2; }
  char const* const src = repack ? argv[2] : argv[1];
  char const* const dst = repack ? argv[3] : argv[2];
  Result r = repack ? RepackBundle(src, dst) : ConvertToAndroid(src, dst);
  std::printf("status=%s\nmessage=%s\nsourcePlatform=%s\nseen=%d retargeted=%d outBytes=%llu\n",
              std::string(StatusText(r.status)).c_str(), r.message.c_str(), r.sourcePlatform.c_str(),
              r.serializedFilesSeen, r.serializedFilesRetargeted,
              (unsigned long long)r.outputBytes);
  return r.ok() ? 0 : 1;
}
