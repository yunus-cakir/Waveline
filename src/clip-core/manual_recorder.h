#pragma once
#include "packet.h"

#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace wl {

struct RecorderConfig {
    std::wstring basePath;  // without extension, e.g. E:\Records\cs2__2026-11-22__20-15-00
    com_ptr<IMFMediaType> videoType;
    std::vector<com_ptr<IMFMediaType>> audioTypes;
    std::vector<std::string> audioTrackNames;
    double splitMinutes = 0;  // 0 = one file
    std::function<void(const std::wstring& path)> onFileReady;  // called on the writer thread
};

// Manual recording fed by the same encoded packets as the replay buffer (no second
// encoder). Every packet is written twice, sequentially:
//  - to the final MP4 (ready the moment recording stops, no conversion step), and
//  - to ~10 s self-contained MP4 segments in "<name>.parts\" as a crash backup: an MP4 is
//    only playable after it is finalised, so if the program or PC crashes the final file is
//    lost but every finished segment stays playable (at most ~10 s lost).
// The backup is deleted when the final file closes normally. (Windows' fragmented-MP4 sink
// would avoid the double write but does not accept HEVC, and re-joining segments with the
// MF source reader scrambles the track order.)
class ManualRecorder {
public:
    ~ManualRecorder();
    void Start(RecorderConfig config);
    void PushVideo(const Packet& p);
    void PushAudio(UINT track, const Packet& p);
    void Stop();  // flushes and closes; conversion continues in the background

    double Seconds() const { return seconds_; }
    int FileCount() const { return fileIndex_; }

private:
    void WriterLoop();
    void WritePacket(int stream, const Packet& p);  // stream 0 = video, 1.. = audio
    void StartGroup(int64_t pts);                   // a group = one final output file
    void FinishGroup();
    void OpenSegment(int64_t pts);
    void CloseSegment();

    RecorderConfig config_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::vector<std::deque<Packet>> queues_;  // [0] video, [1..] audio
    std::vector<int64_t> lastPts_;
    bool stopping_ = false;

    // writer-thread state
    com_ptr<IMFSinkWriter> writer_;       // current backup segment
    std::vector<DWORD> streams_;
    com_ptr<IMFSinkWriter> finalWriter_;  // current output file
    std::vector<DWORD> finalStreams_;
    std::wstring partsDir_, finalPath_;
    size_t segmentCount_ = 0;
    int64_t groupStartPts_ = 0, segStartPts_ = 0;
    bool groupOpen_ = false;
    std::atomic<int> fileIndex_{0};
    std::atomic<double> seconds_{0};
    int64_t firstPts_ = -1;
};

}  // namespace wl
