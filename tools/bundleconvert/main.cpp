#include "VivifyBundleConvert.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Vivify::BundleConvert;

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: conv <src> <dst>\n"); return 2; }
  Result r = ConvertToAndroid(argv[1], argv[2]);
  std::printf("status=%s\nmessage=%s\nsourcePlatform=%s\nseen=%d retargeted=%d outBytes=%llu\n",
              std::string(StatusText(r.status)).c_str(), r.message.c_str(), r.sourcePlatform.c_str(),
              r.serializedFilesSeen, r.serializedFilesRetargeted,
              (unsigned long long)r.outputBytes);
  return r.ok() ? 0 : 1;
}
