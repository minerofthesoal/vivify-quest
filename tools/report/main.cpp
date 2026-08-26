// Host tests for the on-device report writer.
//
// This file is the only thing a player will be asked to send, so it has to
// survive the cases that matter: a missing directory, repeated appends, and a
// long-lived install that would otherwise grow without limit.
#include "VivifyReport.hpp"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace {
int passed = 0;
int failed = 0;

void Check(char const* name, bool ok, std::string const& detail = {}) {
  if (ok) {
    std::printf("ok   %s\n", name);
    passed++;
  } else {
    std::printf("FAIL %s %s\n", name, detail.c_str());
    failed++;
  }
}

std::string ReadAll(std::filesystem::path const& path) {
  std::ifstream in(path, std::ios::binary);
  if (!in) return {};
  return std::string((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
}  // namespace

int main() {
  auto const root = std::filesystem::temp_directory_path() / "vivify-report-tests";
  std::error_code ec;
  std::filesystem::remove_all(root, ec);

  // The mod's data directory will not exist on a fresh install.
  auto const nested = root / "does" / "not" / "exist" / "VivifyReport.txt";
  Vivify::Report::SetFilePathForTesting(nested.string());
  Vivify::Report::Append("LEVEL STARTED", "phase: cache assets 12ms\n");
  Check("creates missing directories", std::filesystem::exists(nested));

  std::string text = ReadAll(nested);
  Check("writes the title", text.find("LEVEL STARTED") != std::string::npos, text);
  Check("writes the body", text.find("cache assets 12ms") != std::string::npos, text);
  Check("writes a separator", text.find("======") != std::string::npos);

  // A level start and a level end are two blocks, not one overwritten.
  Vivify::Report::Append("LEVEL ENDED", "outcome: quit\n");
  text = ReadAll(nested);
  Check("appends rather than overwriting",
        text.find("LEVEL STARTED") != std::string::npos &&
        text.find("LEVEL ENDED") != std::string::npos, text);

  // A body without a trailing newline must not run into the next separator.
  Vivify::Report::Append("NO TRAILING NEWLINE", "last line has none");
  text = ReadAll(nested);
  Check("terminates a body that lacks a newline",
        text.find("last line has none\n") != std::string::npos);

  // Long-lived install: the file must stop growing.
  auto const capped = root / "capped.txt";
  Vivify::Report::SetFilePathForTesting(capped.string());
  std::string const chunk(8000, 'x');
  for (int i = 0; i < 200; i++) {
    Vivify::Report::Append("BULK", chunk);
  }
  auto const size = std::filesystem::file_size(capped, ec);
  Check("file is capped", !ec && size <= Vivify::Report::kMaxBytes,
        std::to_string(size) + " bytes");
  text = ReadAll(capped);
  Check("trim keeps the newest entries", text.find("BULK") != std::string::npos);
  Check("trim says it dropped older entries",
        text.find("earlier entries dropped") != std::string::npos);

  // A path that cannot be written must not throw into the caller.
  Vivify::Report::SetFilePathForTesting("/proc/definitely/not/writable/report.txt");
  bool threw = false;
  try {
    Vivify::Report::Append("UNWRITABLE", "should be swallowed");
  } catch (...) {
    threw = true;
  }
  Check("an unwritable path never throws", !threw);

  std::filesystem::remove_all(root, ec);
  std::printf("\n%d/%d passed\n", passed, passed + failed);
  return failed ? 1 : 0;
}
