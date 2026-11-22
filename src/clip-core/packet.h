#pragma once
#include "common.h"

#include <memory>
#include <vector>

namespace wl {

// One compressed access unit (a video frame or an AAC frame). The payload is shared so a
// snapshot of a 10 minute ring buffer copies pointers, not gigabytes.
struct Packet {
    int64_t pts = 0;        // 100 ns, on the recording timeline
    int64_t duration = 0;   // 100 ns
    bool keyframe = false;
    std::shared_ptr<const std::vector<uint8_t>> data;
};

}  // namespace wl
