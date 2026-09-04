#include "VivifyBundleConvert.hpp"
#include <cstdio>
#include <cstdlib>
#include <string>

using namespace Vivify::BundleConvert;

int main(int argc, char** argv) {
  if (argc < 3) {
    std::fprintf(stderr, "usage: conv [--repack|--shaders] <src> <dst>\n");
    return 2;
  }
  // --repack runs the bundle through the step-4 rewrite path with no shader
  // edits, which must leave a bundle that reads back the same.
  // --shaders runs the whole conversion: translate the DirectX programs to
  // GLSL ES and rebuild the archive around the shaders that changed size.
  std::string const first = argv[1];
  bool const repack = first == "--repack";
  bool const shaders = first == "--shaders";
  bool const flagged = repack || shaders;
  if (flagged && argc < 4) {
    std::fprintf(stderr, "usage: conv %s <src> <dst>\n", first.c_str());
    return 2;
  }
  char const* const src = flagged ? argv[2] : argv[1];
  char const* const dst = flagged ? argv[3] : argv[2];

  if (shaders) {
    ShaderConversion c = ConvertShadersToGles(src, dst);
    std::printf("status=%s\nmessage=%s\nseen=%d translated=%d leftAlone=%d refused=%d "
                "programs=%d outBytes=%llu\ntexSeen=%d texReadable=%d texStreamed=%d\n",
                std::string(StatusText(c.status)).c_str(), c.message.c_str(), c.shadersSeen,
                c.shadersTranslated, c.shadersLeftAlone, c.shadersRefused, c.programsTranslated,
                (unsigned long long)c.outputBytes, c.texturesSeen, c.texturesMarkedReadable,
                c.texturesStreamed);
    for (auto const& refusal : c.refusals) std::printf("refusal=%s\n", refusal.c_str());
    return c.ok() ? 0 : 1;
  }

  Result r = repack ? RepackBundle(src, dst) : ConvertToAndroid(src, dst);
  std::printf("status=%s\nmessage=%s\nsourcePlatform=%s\nseen=%d retargeted=%d outBytes=%llu\n",
              std::string(StatusText(r.status)).c_str(), r.message.c_str(), r.sourcePlatform.c_str(),
              r.serializedFilesSeen, r.serializedFilesRetargeted,
              (unsigned long long)r.outputBytes);
  return r.ok() ? 0 : 1;
}
