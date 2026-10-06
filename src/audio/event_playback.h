#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <variant>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/midi_audio_engine.h"
#include "editor/tempo_map.h"
#include "editor/util.h"
#include "midi/events/meta_event.h"
#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::audio {

using andromeda::editor::MIDITick;
using andromeda::editor::MIDITickAtomic;

struct NoteOnEv {
    std::uint8_t channel, key, velocity;
    bool operator==(const NoteOnEv&) const = default;
};
struct NoteOffEv {
    std::uint8_t channel, key, velocity;
    bool operator==(const NoteOffEv&) const = default;
};
struct ControlEv {
    std::uint8_t channel, controller, value;
    bool operator==(const ControlEv&) const = default;
};
struct PitchBendEv {
    std::uint8_t channel, lsb, msb;
    bool operator==(const PitchBendEv&) const = default;
};

using MidiEvent = std::variant<NoteOnEv, NoteOffEv, ControlEv, PitchBendEv>;

struct TicksTime {
    MIDITick value;
};
struct SecondsTime {
    float value;
};
using MIDITimeType = std::variant<TicksTime, SecondsTime>;

std::optional<int> compare_time(const MIDITimeType& a, const MIDITimeType& b);

struct Scheduled {
    MIDITimeType time;
    MidiEvent event;

    [[nodiscard]] const MIDITimeType& get_time() const { return time; }
};

class ScheduledSequence {
public:
    explicit ScheduledSequence(std::size_t scheduled_at_least) {
        sequence.reserve(scheduled_at_least);
    }

    void insert(Scheduled scheduled_event);

    [[nodiscard]] const Scheduled* peek() const {
        return sequence.empty() ? nullptr : &sequence.front();
    }

    std::optional<Scheduled> pop();

    void clear() { sequence.clear(); }

    [[nodiscard]] std::size_t size() const { return sequence.size(); }

    std::vector<Scheduled> sequence;

private:
    static bool earlier(const Scheduled& a, const Scheduled& b);

    void sift_up(std::size_t index);
    void sift_down(std::size_t index);
};

struct NotePriorities {
    std::vector<std::uint32_t> track_notes;
    std::vector<std::uint16_t> track_rank;
    std::vector<bool> art_track;
    editor::MIDITick short_ticks = 240;

    static NotePriorities build(std::vector<midi::MIDITrack>& tracks, std::uint16_t ppq);
};

[[nodiscard]] int live_playback_threads();

struct TransportClock {
    std::int64_t epoch_ns = 0;
    double start_secs = 0.0;
    bool running = false;
};

class PlaybackManager : public AudioEngine,
                        public std::enable_shared_from_this<PlaybackManager> {
public:
    PlaybackManager(SharedDevice device,
                    std::vector<midi::MIDITrack>* tracks,
                    std::vector<midi::MetaEvent>* meta_events,
                    editor::TempoMap* tempo_map);

    void on_event(const app::AndromedaEvent& event) override;

    void navigate_to(MIDITick tick) override;
    [[nodiscard]] MIDITick get_playback_ticks() const override;
    [[nodiscard]] MIDITick get_playback_start_tick() const override { return playback_start_pos; }
    void start_playback() override;
    void stop() override;
    void toggle_playback() override;
    [[nodiscard]] bool is_playing() const override { return playing; }
    void switch_device(SharedDevice device) override;
    void reset_events() override;
    void start_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) override;
    void update_play_at_mouse(std::uint8_t key, std::uint8_t channel,
                              std::uint8_t velocity) override;
    void stop_play_at_mouse(std::uint8_t key, std::uint8_t channel) override;

    void prewarm_priorities();

    std::vector<midi::MetaEvent>* meta_events;
    std::vector<midi::MIDITrack>* tracks;
    SharedDevice device;
    std::uint16_t ppq = 960;

    bool playing = false;
    MIDITick playback_start_pos = 0;
    std::shared_ptr<MIDITickAtomic> playback_pos_ticks;

private:
    std::shared_ptr<std::atomic<bool>> stop_playback_;

    // target tick + 1, 0 means none; only the audio thread may move the cursors
    std::shared_ptr<std::atomic<std::int64_t>> seek_request_;

    editor::TempoMap* tempo_map_;

    std::shared_future<std::shared_ptr<const NotePriorities>> priorities_;
    std::uint64_t priorities_signature_ = 0;

    std::shared_future<std::shared_ptr<const NotePriorities>> ensure_priorities(
        std::vector<midi::MIDITrack>& trks, std::uint16_t ppq_copy,
        bool background = false);

    std::shared_ptr<util::SharedMut<TransportClock>> transport_clock_ =
        std::make_shared<util::SharedMut<TransportClock>>();

    float start_pos_secs_from_ticks_ = 0.0f;

    bool play_at_mouse_ = false;
    std::uint8_t mouse_last_key_ = 0;
};

}
