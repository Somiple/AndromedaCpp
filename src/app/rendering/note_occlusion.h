#pragma once

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <stop_token>
#include <thread>
#include <unordered_map>
#include <vector>

#include "editor/midi_types.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::app::rendering {

class NoteOcclusionStore {
public:
    struct Culled {
        std::vector<midi::Note> notes;
        editor::MIDITick max_length = 0;
    };

    NoteOcclusionStore() = default;

    NoteOcclusionStore(const NoteOcclusionStore&) = delete;
    NoteOcclusionStore& operator=(const NoteOcclusionStore&) = delete;

    static void set_enabled(bool on) { enabled_ = on; }
    [[nodiscard]] static bool enabled() { return enabled_; }

    void attach(util::SharedPtr<std::vector<midi::MIDITrack>> tracks);

    const Culled* get(std::size_t track, std::uint64_t revision,
                      const std::vector<midi::Note>& notes);

    void clear();

    [[nodiscard]] std::size_t cached_notes() const;

private:
    enum class State : std::uint8_t {
        Idle,
        Queued,
        Built,
        NotWorthIt,
    };

    struct Entry {
        std::uint64_t revision = 0;
        Culled culled;
        State state = State::Idle;
    };

    struct Job {
        std::size_t track = 0;
        std::uint64_t revision = 0;
    };

    static Culled cull(const std::vector<midi::Note>& src);

    void run(std::stop_token stop);

    static constexpr double KEEP_BELOW = 0.75;

    static constexpr std::size_t MAX_CACHED_NOTES = 180u << 20;

    inline static bool enabled_ = true;

    util::SharedPtr<std::vector<midi::MIDITrack>> tracks_;

    mutable std::mutex mutex_;
    std::condition_variable_any wake_;
    std::deque<Job> queue_;
    std::unordered_map<std::size_t, Entry> entries_;
    std::size_t cached_notes_ = 0;

    // must stay the last member so it is joined before what it uses is destroyed
    std::jthread worker_;
};

}
