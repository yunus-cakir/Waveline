#include "replay_ring.h"

#include <algorithm>

namespace wl {

namespace {
uint64_t SizeOf(const Packet& p) { return p.data ? p.data->size() : 0; }
}

void ReplayRing::PushVideo(Packet&& p) {
    std::lock_guard lock(mutex_);
    if (video_.empty() && !p.keyframe) return;  // always start on a keyframe
    bytes_ += SizeOf(p);
    video_.push_back(std::move(p));
    TrimLocked();
}

void ReplayRing::PushAudio(UINT track, Packet&& p) {
    std::lock_guard lock(mutex_);
    if (track >= audio_.size()) return;
    bytes_ += SizeOf(p);
    audio_[track].push_back(std::move(p));
    TrimLocked();
}

void ReplayRing::TrimLocked() {
    if (video_.empty()) return;
    const int64_t newest = video_.back().pts;
    const int64_t keepFrom = newest - keepHns_.load();

    // Drop whole GOPs while the *next* keyframe is still old enough to start a full clip.
    for (;;) {
        size_t next = 1;
        while (next < video_.size() && !video_[next].keyframe) ++next;
        if (next >= video_.size() || video_[next].pts > keepFrom) break;
        for (size_t i = 0; i < next; ++i) {
            bytes_ -= SizeOf(video_.front());
            video_.pop_front();
        }
    }
    const int64_t oldestVideo = video_.front().pts;
    for (auto& track : audio_) {
        while (!track.empty() && track.front().pts + track.front().duration < oldestVideo) {
            bytes_ -= SizeOf(track.front());
            track.pop_front();
        }
    }
}

bool ReplayRing::TakeSnapshot(int64_t durationHns, Snapshot& out) const {
    std::lock_guard lock(mutex_);
    if (video_.empty()) return false;
    const int64_t want = video_.back().pts + video_.back().duration - durationHns;

    // Last keyframe at or before `want`; if the buffer is shorter, the first packet.
    size_t start = 0;
    for (size_t i = 0; i < video_.size(); ++i) {
        if (video_[i].pts > want) break;
        if (video_[i].keyframe) start = i;
    }
    out.startPts = video_[start].pts;

    // Audio runs ~150 ms behind real time, so its newest packet ends before the newest
    // video frame. End the clip where every track still has data.
    int64_t end = video_.back().pts + video_.back().duration;
    for (const auto& track : audio_)
        if (!track.empty()) end = std::min(end, track.back().pts + track.back().duration);

    for (size_t i = start; i < video_.size() && video_[i].pts + video_[i].duration <= end; ++i)
        out.video.push_back(video_[i]);
    if (out.video.empty()) out.video.push_back(video_[start]);

    out.audio.assign(audio_.size(), {});
    for (size_t t = 0; t < audio_.size(); ++t)
        for (const auto& p : audio_[t])
            if (p.pts >= out.startPts && p.pts < end) out.audio[t].push_back(p);
    return true;
}

int64_t ReplayRing::BufferedHns() const {
    std::lock_guard lock(mutex_);
    if (video_.empty()) return 0;
    return video_.back().pts + video_.back().duration - video_.front().pts;
}

}  // namespace wl
