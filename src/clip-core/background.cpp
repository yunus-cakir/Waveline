#include "background.h"

namespace wl {

BackgroundTasks& BackgroundTasks::Instance() {
    static BackgroundTasks instance;
    return instance;
}

void BackgroundTasks::Run(std::function<void()> job) {
    std::lock_guard lock(mutex_);
    ++running_;
    threads_.emplace_back([this, job = std::move(job)] {
        SetThreadPriority(GetCurrentThread(), THREAD_MODE_BACKGROUND_BEGIN);
        winrt::init_apartment(winrt::apartment_type::multi_threaded);
        try {
            job();
        } catch (const HrError& e) {
            Warn("Arka plan isi basarisiz: %s (0x%08X)", e.what(), static_cast<unsigned>(e.hr));
        } catch (const std::exception& e) {
            Warn("Arka plan isi basarisiz: %s", e.what());
        }
        std::lock_guard l(mutex_);
        --running_;
    });
}

size_t BackgroundTasks::Pending() {
    std::lock_guard lock(mutex_);
    return running_;
}

void BackgroundTasks::JoinAll() {
    std::vector<std::thread> threads;
    {
        std::lock_guard lock(mutex_);
        threads.swap(threads_);
    }
    for (auto& t : threads)
        if (t.joinable()) t.join();
}

}  // namespace wl
