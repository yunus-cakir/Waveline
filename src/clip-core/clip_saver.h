#pragma once
#include "replay_ring.h"

#include <mfapi.h>
#include <mfidl.h>

#include <string>
#include <vector>

namespace wl {

// Written into the MP4: iTunes-style tags (title, album/genre = game, tool, date, comment)
// shown by players/editors, and Microsoft's "WM/Category", which Windows Explorer shows
// as the file's Tags.
//
// Waveline's own keys are iTunes "freeform" items under the domain com.waveline.clip, so
// they travel with the file (moved/renamed/copied) and a future library can find them:
//   id    unique id of this file (survives moves and renames)
//   kind  "clip" | "recording"
//   game  game name (same as album, kept separately in case a tagger edits album)
//   edit  JSON, non-destructive edit state for the future library/editor:
//         {"version":1,"favorite":false,"trimStart":null,"trimEnd":null,
//          "tracks":[{"name":"Mix","volume":1.0,"muted":false},...]}
struct ClipMetadata {
    std::wstring title;    // e.g. "Counter-Strike 2 – 22.11.2026 20:15"
    std::wstring game;     // e.g. "Counter-Strike 2"
    std::wstring comment;  // e.g. "Waveline Clip – geriye dönük klip"
    std::wstring date;     // ISO 8601 local time; empty = now
    std::wstring id;
    std::wstring kind;
    std::string editJson;  // UTF-8 JSON
    bool Empty() const { return title.empty() && game.empty() && comment.empty() && id.empty(); }
};

std::wstring NewClipId();  // random UUID, lower-case, no braces
std::string DefaultEditJson(const std::vector<std::string>& trackNames);

// Reads / replaces only the metadata (e.g. to toggle "favorite" later). The MP4's 'moov'
// must be the last box, which is true for every file Waveline writes.
bool Mp4ReadMetadata(const std::wstring& path, ClipMetadata& out);
bool Mp4WriteMetadata(const std::wstring& path, const ClipMetadata& metadata);

struct ClipSaveInput {
    std::wstring path;
    ReplayRing::Snapshot snapshot;
    com_ptr<IMFMediaType> videoType;
    std::vector<com_ptr<IMFMediaType>> audioTypes;
    std::vector<std::string> audioTrackNames;  // written as MP4 handler names
    ClipMetadata metadata;
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
// Also writes `metadata` (replacing any existing 'udta').
bool Mp4SetTrackNames(const std::wstring& path, const std::string& videoName,
                      const std::vector<std::string>& audioNames, const ClipMetadata& metadata = {});

}  // namespace wl
