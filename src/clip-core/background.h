#pragma once
#include "common.h"

#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace wl {

// Low-priority jobs that must finish before the program exits (e.g. converting a finished
// recording to a regular MP4) but must not block the recording loop.
class BackgroundTasks {
public:
    static BackgroundTasks& Instance();
    void Run(std::function<void()> job);
    void JoinAll();
    size_t Pending();

private:
    std::mutex mutex_;
    std::vector<std::thread> threads_;
    size_t running_ = 0;
};

}  // namespace wl
