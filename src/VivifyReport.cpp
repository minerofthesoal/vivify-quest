#include "VivifyReport.hpp"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <vector>

namespace Vivify::Report {
namespace {

std::mutex& FileMutex() {
  static std::mutex mutex;
  return mutex;
}

std::string& PathStorage() {
  static std::string path =
      "/sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyReport.txt";
  return path;
}

std::string Timestamp() {
  auto const now = std::chrono::system_clock::now();
  auto const seconds = std::chrono::system_clock::to_time_t(now);
  std::tm parts{};
#if defined(_WIN32)
  localtime_s(&parts, &seconds);
#else
  localtime_r(&seconds, &parts);
#endif
  char buffer[32];
  if (std::strftime(buffer, sizeof(buffer), "%Y-%m-%d %H:%M:%S", &parts) == 0) {
    return "unknown-time";
  }
  return buffer;
}

// Drops the oldest blocks once the file gets large, so leaving the mod
// installed for months cannot fill a headset. The cut lands on a line boundary
// so the surviving text still reads cleanly.
void TrimIfLarge(std::filesystem::path const& path) {
  std::error_code ec;
  auto const size = std::filesystem::file_size(path, ec);
  if (ec || size <= kMaxBytes) return;

  std::ifstream input(path, std::ios::binary);
  if (!input) return;
  input.seekg(static_cast<std::streamoff>(size - kKeepBytes), std::ios::beg);
  if (!input) return;
  std::string kept((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
  input.close();

  auto const firstNewline = kept.find('\n');
  if (firstNewline != std::string::npos) kept.erase(0, firstNewline + 1);

  // Written beside the target and renamed, so an interrupted trim cannot leave
  // the report truncated to nothing.
  auto const temporary = std::filesystem::path(path).concat(".trim");
  {
    std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
    if (!output) return;
    output << "[earlier entries dropped to keep this file small]\n" << kept;
    if (!output) return;
  }
  std::filesystem::rename(temporary, path, ec);
  if (ec) std::filesystem::remove(temporary, ec);
}

}  // namespace

std::string const& FilePath() {
  return PathStorage();
}

void SetFilePathForTesting(std::string path) {
  std::lock_guard<std::mutex> lock(FileMutex());
  PathStorage() = std::move(path);
}

void Append(std::string_view title, std::string_view body) {
  // A diagnostic file must never be the reason the game breaks, so every
  // failure here is swallowed: no exception escapes into Unity, and a report
  // that cannot be written simply is not written.
  try {
    std::lock_guard<std::mutex> lock(FileMutex());
    std::filesystem::path const path(PathStorage());

    std::error_code ec;
    if (path.has_parent_path()) {
      std::filesystem::create_directories(path.parent_path(), ec);
      if (ec) return;
    }
    TrimIfLarge(path);

    std::ofstream output(path, std::ios::binary | std::ios::app);
    if (!output) return;
    output << "\n================================================================\n"
           << "[" << Timestamp() << "] " << title << "\n"
           << "----------------------------------------------------------------\n"
           << body;
    if (!body.empty() && body.back() != '\n') output << "\n";
    output.flush();
  } catch (...) {
    // Intentionally ignored -- see above.
  }
}

}  // namespace Vivify::Report
