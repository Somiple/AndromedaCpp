#include "app/rendering/note_uploader.h"

#include <chrono>
#include <format>
#include <shared_mutex>
#include <utility>

#include <glad/glad.h>
// glad must be included before glfw
#include <GLFW/glfw3.h>

#include "util/debugger.h"

namespace andromeda::app::rendering {

using util::Debugger;

NoteUploader::~NoteUploader() {
    stop();

    if (context_ != nullptr) {
        glfwDestroyWindow(context_);
        context_ = nullptr;
    }
}

bool NoteUploader::init(GLFWwindow* share, std::shared_ptr<NoteGpuCache> cache) {
    if (share == nullptr || !cache) {
        return false;
    }

    cache_ = std::move(cache);

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    context_ = glfwCreateWindow(1, 1, "andromeda-uploader", nullptr, share);

    glfwDefaultWindowHints();

    if (context_ == nullptr) {
        Debugger::log_warning(
            "No shared GL context for the note uploader; residency stays lazy.");
        return false;
    }

    return true;
}

void NoteUploader::stop() {
    cancel_.store(true, std::memory_order_relaxed);
    if (thread_.joinable()) {
        thread_.join();
    }
    cancel_.store(false, std::memory_order_relaxed);
    running_.store(false, std::memory_order_relaxed);
}

void NoteUploader::prewarm(util::SharedPtr<std::vector<midi::MIDITrack>> tracks) {
    if (context_ == nullptr || !cache_ || !tracks || !NoteGpuCache::enabled()) {
        return;
    }

    stop();

    running_.store(true, std::memory_order_relaxed);
    uploaded_tracks_.store(0, std::memory_order_relaxed);

    cache_->begin_background_pass();

    thread_ = std::thread([this, tracks = std::move(tracks)]() mutable { run(std::move(tracks)); });
}

void NoteUploader::run(util::SharedPtr<std::vector<midi::MIDITrack>> tracks) {
    glfwMakeContextCurrent(context_);

    const auto started = std::chrono::steady_clock::now();
    std::size_t done = 0;
    std::size_t notes = 0;

    constexpr std::size_t PUBLISH_BYTES = 64ull * 1024ull * 1024ull;
    std::vector<NoteGpuCache::Written> batch;
    std::size_t batch_bytes = 0;
    const auto flush_batch = [&] {
        cache_->publish(batch);
        batch_bytes = 0;
    };

    const auto upload_wanted = [&](std::size_t t) {
        std::shared_lock lock(tracks->mutex);
        if (t >= tracks->value.size() || tracks->value[t].get_notes().empty()) {
            return true;
        }
        const midi::MIDITrack& track = tracks->value[t];
        const std::size_t key = NoteGpuCache::key_for(t, NoteGpuCache::Variant::Exact);
        const NoteGpuCache::Prewarm result = cache_->prewarm(key, track.get_notes(), track.revision);
        if (result == NoteGpuCache::Prewarm::Written) {
            batch.push_back({key, track.revision});
        }
        return result != NoteGpuCache::Prewarm::Full;
    };

    for (std::size_t t = 0;; ++t) {
        if (cancel_.load(std::memory_order_relaxed)) {
            break;
        }

        bool full = false;
        const std::vector<std::size_t> wanted_tracks = cache_->take_wanted_tracks();
        for (const std::size_t wanted : wanted_tracks) {
            if (cancel_.load(std::memory_order_relaxed) || !upload_wanted(wanted)) {
                full = !cancel_.load(std::memory_order_relaxed);
                break;
            }
        }
        if (!wanted_tracks.empty()) {
            flush_batch();
        }
        if (full || cancel_.load(std::memory_order_relaxed)) {
            break;
        }

        std::size_t key = 0;
        std::uint64_t revision = 0;
        bool have_track = false;
        {
            std::shared_lock lock(tracks->mutex);
            if (t >= tracks->value.size()) {
                break;
            }

            const midi::MIDITrack& track = tracks->value[t];
            if (track.get_notes().empty()) {
                continue;
            }

            key = NoteGpuCache::key_for(t, NoteGpuCache::Variant::Exact);
            revision = track.revision;

            // prewarm must run under the read lock; it keeps the notes alive during the copy
            const NoteGpuCache::Prewarm result =
                cache_->prewarm(key, track.get_notes(), revision);
            have_track = result != NoteGpuCache::Prewarm::Full;
            if (result == NoteGpuCache::Prewarm::Written) {
                batch.push_back({key, revision});
                batch_bytes += track.get_notes().size() * sizeof(midi::Note);
            }
            if (have_track && result != NoteGpuCache::Prewarm::Skipped) {
                notes += track.get_notes().size();
            }
        }

        if (!have_track) {
            break;
        }

        if (batch_bytes >= PUBLISH_BYTES) {
            flush_batch();
        }

        done += 1;
        uploaded_tracks_.store(done, std::memory_order_relaxed);
    }

    flush_batch();

    cache_->end_background_pass();

    glfwMakeContextCurrent(nullptr);

    running_.store(false, std::memory_order_relaxed);

    if (!cancel_.load(std::memory_order_relaxed)) {
        const double ms = std::chrono::duration<double, std::milli>(
                              std::chrono::steady_clock::now() - started)
                              .count();
        Debugger::log(std::format("Prerendered {} tracks ({} notes) to the GPU in {:.0f} ms", done,
                                  notes, ms));
    }
}

}
