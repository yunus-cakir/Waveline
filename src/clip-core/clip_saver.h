#pragma once
#include "replay_ring.h"

#include <mfapi.h>
#include <mfidl.h>

#include <string>
#include <vector>

namespace wl {

struct ClipSaveInput {
    std::wstring path;
    ReplayRing::Snapshot snapshot;
    com_ptr<IMFMediaType> videoType;
    std::vector<com_ptr<IMFMediaType>> audioTypes;
    std::vector<std::string> audioTrackNames;  // written as MP4 handler names
};

struct ClipSaveResult {
    double seconds = 0;
    uint64_t bytes = 0;
    bool namesWritten = false;
};

// Writes a snapshot to MP4 without re-encoding (Media Foundation sink writer in
// pass-through mode), then names the audio tracks.
ClipSaveResult SaveClip(const ClipSaveInput& input);

// Rewrites the 'hdlr' names of an MP4 whose 'moov' box is at the end of the file.
// Track order: video first, then audio tracks in order. Returns false if not possible.
bool Mp4SetTrackNames(const std::wstring& path, const std::string& videoName,
                      const std::vector<std::string>& audioNames);

}  // namespace wl
