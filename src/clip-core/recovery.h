#pragma once
#include "common.h"

#include <mfapi.h>
#include <mfidl.h>

#include <functional>
#include <string>
#include <vector>

namespace wl {

// Stored next to the backup segments when a recording starts, so an interrupted recording
// can be rebuilt later: the exact media types (incl. codec headers) and track names.
void WriteRecoveryInfo(const std::wstring& partsDir, IMFMediaType* video,
                       const std::vector<com_ptr<IMFMediaType>>& audio, const std::vector<std::string>& names);

// Finds "<name>.parts" folders left behind by a crash in `dir` and rebuilds "<name>.mp4"
// from every complete segment (the segment being written at the crash is unreadable and
// skipped). Segments are parsed with our own minimal MP4 reader, so the track order is
// exactly the written one. Calls `onRecovered(path, seconds)` per rebuilt file.
void RecoverInterruptedRecordings(const std::wstring& dir,
                                  const std::function<void(const std::wstring&, double)>& onRecovered);

}  // namespace wl
