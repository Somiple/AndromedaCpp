#pragma once

#include <atomic>
#include <cstddef>
#include <memory>
#include <thread>
#include <vector>

#include "app/rendering/note_gpu_cache.h"
#include "midi/midi_track.h"
#include "util/shared.h"

struct GLFWwindow;

namespace andromeda::app::rendering {

class NoteUploader {
public:
    NoteUploader() = default;
    ~NoteUploader();

    NoteUploader(const NoteUploader&) = delete;
    NoteUploader& operator=(const NoteUploader&) = delete;

    bool init(GLFWwindow* share, std::shared_ptr<NoteGpuCache> cache);

    void prewarm(std::vector<midi::MIDITrack>* tracks);

    void stop();

    [[nodiscard]] bool running() const { return running_.load(std::memory_order_relaxed); }
    [[nodiscard]] std::size_t uploaded_tracks() const {
        return uploaded_tracks_.load(std::memory_order_relaxed);
    }

private:
    void run(std::vector<midi::MIDITrack>* tracks);

    GLFWwindow* context_ = nullptr;
    std::shared_ptr<NoteGpuCache> cache_;
    std::thread thread_;
    std::atomic<bool> cancel_{false};
    std::atomic<bool> running_{false};
    std::atomic<std::size_t> uploaded_tracks_{0};
};

}
