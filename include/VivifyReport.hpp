#pragma once

// A plain-text diagnostic file Vivify writes for itself, separate from paperlog.
//
// paperlog output lives somewhere most players cannot reach and cannot be
// retrieved without adb, so "send me the log" is not a reasonable thing to ask.
// This writes one human-readable block per level, to a fixed path under the
// mod's own data directory that any file browser or MTP connection can see:
//
//   /sdcard/ModData/com.beatgames.beatsaber/Mods/Vivify/VivifyReport.txt
//
// A block is appended when a level starts and again when it ends, so a level
// that freezes still leaves its "started" block behind -- which is the whole
// point, since a frozen game never reaches the end-of-level write.
//
// Deliberately Unity-free so it can be exercised on a host compiler, and
// deliberately incapable of throwing into the game: every failure is swallowed.

#include <string>
#include <string_view>

namespace Vivify::Report {

// Where the report is written. Also creates the directory on first use.
std::string const& FilePath();

// Appends one block, prefixed with a separator and a wall-clock timestamp.
// Never throws; a write failure is silently dropped rather than risking the
// game over a diagnostic file.
void Append(std::string_view title, std::string_view body);

// Overrides the destination. Only used by the host tests.
void SetFilePathForTesting(std::string path);

// Largest the file is allowed to get before the oldest blocks are dropped.
// A player leaving this on for months must not fill their headset.
inline constexpr size_t kMaxBytes = 512u * 1024u;
inline constexpr size_t kKeepBytes = 256u * 1024u;

}  // namespace Vivify::Report
