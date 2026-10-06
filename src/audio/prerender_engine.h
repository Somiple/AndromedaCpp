#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

#include "audio/audio_engine.h"
#include "audio/event_playback.h"
#include "audio/prerender_engine/limiter.h"
#include "audio/soft_synth.h"
#include "editor/tempo_map.h"
#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::audio {

inline constexpr std::size_t AUDIO_SAMPLERATE = 48000;
inline constexpr std::size_t AUDIO_CHANNELS = 2;
inline constexpr std::size_t AUDIO_BUFFER_SECONDS = 30;
inline constexpr std::size_t AUDIO_BUFFER_SIZE =
    AUDIO_SAMPLERATE * AUDIO_CHANNELS * AUDIO_BUFFER_SECONDS;

class PrerenderEngine : public AudioEngine {
public:
    PrerenderEngine(std::vector<midi::MIDITrack>* tracks,
                    editor::TempoMap* tempo_map);
    ~PrerenderEngine() override;

    PrerenderEngine(const PrerenderEngine&) = delete;
    PrerenderEngine& operator=(const PrerenderEngine&) = delete;

    void on_event(const app::AndromedaEvent& event) override;

    void init() override;
    void navigate_to(editor::MIDITick tick) override;
    [[nodiscard]] editor::MIDITick get_playback_ticks() const override;
    [[nodiscard]] editor::MIDITick get_playback_start_tick() const override;
    void start_playback() override;
    void stop() override;
    void toggle_playback() override;
    [[nodiscard]] bool is_playing() const override;
    void reset_events() override;

    [[nodiscard]] bool is_available() const { return synth_ != nullptr; }

    bool load_soundfonts(const std::vector<std::string>& soundfont_paths);

private:
    void fill_output(float* data, std::size_t sample_count);

    void generate_events(float start_time_secs, editor::MIDITick start_tick);
    void render_audio(float start_time, editor::MIDITick start_tick_time, float speed);

    [[nodiscard]] float get_player_time() const;
    [[nodiscard]] float get_buffer_seconds() const;

    void start_prerender(float start_time, editor::MIDITick start_tick, float speed);
    void stop_prerender();
    void sync_player(float start_time, float speed);
    void kill_last_generators();
    void start_audio(editor::MIDITick time, float speed, bool force);

    bool open_stream();
    void close_stream();
    void output_thread_main();

    std::unique_ptr<SoftSynth> synth_;
    Limiter limiter_{0.010f, 0.1f, static_cast<float>(AUDIO_SAMPLERATE)};
    std::mutex limiter_mutex_;

    std::atomic<std::size_t> read_pos_{0};
    std::atomic<std::size_t> write_pos_{0};
    std::vector<float> audio_buffer_ = std::vector<float>(AUDIO_BUFFER_SIZE, 0.0f);
    std::mutex audio_buffer_mutex_;

    std::atomic<bool> reset_requested_{false};

    std::thread generator_thread_;
    std::thread event_generator_thread_;

    std::map<editor::MIDITick, std::vector<MidiEvent>> events_;
    std::mutex events_mutex_;

    std::vector<midi::MIDITrack>* tracks_;
    editor::TempoMap* tempo_map_;
    float start_time_ = 0.0f;

    std::atomic<std::uint16_t> ppq_{960};
    editor::MIDITick playback_start_ticks_ = 0;

    std::atomic<bool> is_playing_{false};
    bool stream_playing_ = false;

    struct Stream;
    std::unique_ptr<Stream> stream_;
    std::thread output_thread_;
    std::atomic<bool> output_running_{false};
};

}
