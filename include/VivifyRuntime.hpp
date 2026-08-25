#pragma once
#include <functional>
#include <string>

namespace Vivify {
void LateLoad();
void RefreshMultipassRendering();
void RefreshIsolationSettings();

// Progress of a bulk PC->Android bundle conversion pass.
struct BulkConversionProgress {
  int levelsScanned = 0;
  int levelsTotal = 0;
  int converted = 0;
  int alreadyDone = 0;
  int failed = 0;
  bool finished = false;
  // Name of the song folder currently being worked on, or a closing summary
  // once finished is true.
  std::string status;
};

// Converts every installed custom level that has a PC-built Vivify bundle and
// no Android one, without needing the level to be selected or playable.
//
// A map whose only bundle is a PC build has its play button disabled, so there
// is otherwise no way to reach it -- this is the escape hatch for that.
// Work happens on a background thread; onProgress is invoked on the main thread
// and is called a final time with finished == true.
// Converts every installed custom level's PC bundle that has no Android bundle.
//
// With force=true, a level whose conversion is already cached is converted
// again and the cached file replaced. That matters because a cached bundle is
// keyed on the *source* bundle's identity, so a conversion produced by an older
// (or buggier) version of the converter is reused indefinitely -- there is
// otherwise no way to pick up converter fixes short of deleting the cache
// directory by hand.
void StartBulkPcBundleConversion(std::function<void(BulkConversionProgress const&)> onProgress,
                                 bool force = false);

// True while a bulk pass started by StartBulkPcBundleConversion is running.
bool IsBulkPcBundleConversionRunning();
}
