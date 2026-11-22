#pragma once
#include "packet.h"

#include <atomic>
#include <deque>
#include <mutex>
#include <vector>

namespace wl {

// The replay buffer: compressed packets of the last N minutes, kept in RAM.
// Video is always trimmed on keyframe boundaries so a snapshot can start cleanly.
class ReplayRing {
public:
    explicit ReplayRing(UINT audioTracks) : audio_(audioTracks) {}

    void SetDuration(int64_t hns) { keepHns_ = hns; }
    int64_t Duration() const { return keepHns_; }

    void PushVideo(Packet&& p);
    void PushAudio(UINT track, Packet&& p);

    struct Snapshot {
        int64_t startPts = 0;  // pts of the first (key)frame, subtract to rebase to 0
        std::vector<Packet> video;
        std::vector<std::vector<Packet>> audio;
    };
    // The last `durationHns` of material, starting at the keyframe at or just before the
    // requested start (clips are never shorter than asked, at most ~1 GOP longer).
    // `trimToAudio`: end where every audio track still has data (for clips). Manual
    // recording seeds itself without trimming so the live packets continue seamlessly.
    bool TakeSnapshot(int64_t durationHns, Snapshot& out, bool trimToAudio = true) const;

    int64_t BufferedHns() const;
    uint64_t Bytes() const { return bytes_; }

private:
    void TrimLocked();

    mutable std::mutex mutex_;
    std::deque<Packet> video_;
    std::vector<std::deque<Packet>> audio_;
    std::atomic<int64_t> keepHns_{60 * kHns};
    std::atomic<uint64_t> bytes_{0};
};

}  // namespace wl
