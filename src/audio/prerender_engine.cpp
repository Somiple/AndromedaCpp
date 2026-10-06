#include "audio/prerender_engine.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <format>
#include <shared_mutex>
#include <utility>

#include "editor/util.h"
#include "util/debugger.h"

#ifdef _WIN32
#include <windows.h>
// mmsystem must be included after windows.h
#include <mmsystem.h>
#include <mmreg.h>
#endif

namespace andromeda::audio {

using editor::MIDITick;
using util::Debugger;

namespace {

constexpr std::size_t OUTPUT_FRAMES = 2048;
constexpr std::size_t OUTPUT_BUFFERS = 4;

}

#ifdef _WIN32
struct PrerenderEngine::Stream {
    HWAVEOUT handle = nullptr;
    HANDLE event = nullptr;
    std::array<WAVEHDR, OUTPUT_BUFFERS> headers{};
    std::array<std::vector<float>, OUTPUT_BUFFERS> buffers{};
};
#else
struct PrerenderEngine::Stream {};
#endif

PrerenderEngine::PrerenderEngine(std::vector<midi::MIDITrack>* tracks,
                                 editor::TempoMap* tempo_map)
    : tracks_(tracks), tempo_map_(tempo_map) {}

PrerenderEngine::~PrerenderEngine() {
    reset_requested_.store(true, std::memory_order_relaxed);
    is_playing_.store(false, std::memory_order_relaxed);

    if (generator_thread_.joinable()) {
        generator_thread_.join();
    }
    if (event_generator_thread_.joinable()) {
        event_generator_thread_.join();
    }

    close_stream();
}

void PrerenderEngine::on_event(const app::AndromedaEvent& event) {
    if (const auto* ppq_changed = std::get_if<app::PPQChanged>(&event)) {
        ppq_.store(ppq_changed->new_ppq, std::memory_order_relaxed);
    }
}

void PrerenderEngine::init() {
    synth_ = create_soft_synth(static_cast<unsigned int>(AUDIO_SAMPLERATE));
    if (!synth_) {
        return;
    }

    const std::vector<std::string> paths = default_soundfont_paths();
    if (paths.empty() || !load_soundfonts(paths)) {
        Debugger::log_warning(
            "Prerendered audio has no soundfont: set ANDROMEDA_SOUNDFONT or put an .sf2 in "
            "assets/soundfonts.");
        synth_.reset();
        return;
    }

    if (!open_stream()) {
        synth_.reset();
    }
}

bool PrerenderEngine::load_soundfonts(const std::vector<std::string>& soundfont_paths) {
    if (!synth_) {
        return false;
    }

    const bool ok = synth_->load_soundfonts(soundfont_paths);
    if (ok) {
        Debugger::log(
            std::format("Loaded {} soundfonts into prerender engine.", soundfont_paths.size()));
    }
    return ok;
}

bool PrerenderEngine::is_playing() const { return is_playing_.load(std::memory_order_relaxed); }

MIDITick PrerenderEngine::get_playback_ticks() const {
    if (!is_playing()) {
        return get_playback_start_tick();
    }

    const float time_secs = get_player_time();

    return tempo_map_->secs_to_ticks_from_map(ppq_.load(std::memory_order_relaxed),
                                                    time_secs);
}

MIDITick PrerenderEngine::get_playback_start_tick() const { return playback_start_ticks_; }

void PrerenderEngine::start_playback() {
    if (is_playing() || !synth_) {
        return;
    }

    start_audio(playback_start_ticks_, 1.0f, true);

    stream_playing_ = true;
    is_playing_.store(true, std::memory_order_relaxed);
}

void PrerenderEngine::navigate_to(MIDITick tick) {
    playback_start_ticks_ = tick;
    if (is_playing()) {
        start_audio(tick, 1.0f, false);
    }
}

void PrerenderEngine::toggle_playback() {
    if (!is_playing()) {
        start_playback();
    } else {
        stop();
    }
}

void PrerenderEngine::stop() {
    if (!is_playing()) {
        return;
    }

    stop_prerender();
    stream_playing_ = false;
    is_playing_.store(false, std::memory_order_relaxed);
}

void PrerenderEngine::reset_events() {
}

void PrerenderEngine::fill_output(float* data, std::size_t sample_count) {
    const bool paused = !is_playing_.load(std::memory_order_relaxed);
    if (paused || reset_requested_.load(std::memory_order_relaxed)) {
        std::fill_n(data, sample_count, 0.0f);
        return;
    }

    const std::size_t read_pos = read_pos_.load(std::memory_order_relaxed);
    const std::size_t write_pos = write_pos_.load(std::memory_order_relaxed);
    const std::size_t read = read_pos % (AUDIO_BUFFER_SIZE / 2);
    const std::size_t frames = sample_count / 2;

    if (read_pos + frames > write_pos) {
        // fixed rust bug: the subtraction was backwards, wrapped, and replayed stale samples
        std::size_t copy_count = write_pos > read_pos ? write_pos - read_pos : 0;
        if (copy_count > frames) {
            copy_count = frames;
        }

        if (copy_count > 0) {
            std::lock_guard lock(audio_buffer_mutex_);
            for (std::size_t i = 0; i < copy_count * 2; ++i) {
                data[i] = audio_buffer_[(i + read * 2) % AUDIO_BUFFER_SIZE];
            }
        } else {
            copy_count = 0;
        }

        for (std::size_t i = copy_count * 2; i < sample_count; ++i) {
            data[i] = 0.0f;
        }
    } else {
        std::lock_guard lock(audio_buffer_mutex_);
        for (std::size_t i = 0; i < sample_count; ++i) {
            data[i] = audio_buffer_[(i + read * 2) % AUDIO_BUFFER_SIZE];
        }
    }

    read_pos_.fetch_add(frames, std::memory_order_relaxed);

    std::lock_guard lock(limiter_mutex_);
    limiter_.apply_limiter(std::span<float>(data, sample_count));
}

void PrerenderEngine::generate_events(float, MIDITick start_tick) {
    {
        std::lock_guard lock(events_mutex_);
        events_.clear();
    }

    for (midi::MIDITrack& track : *tracks_) {
        if (track.is_empty()) {
            continue;
        }

        const std::vector<midi::Note>& notes = track.get_notes();
        const std::vector<midi::ChannelEvent>& ch_evs = track.get_channel_evs();

        const auto partition =
            std::partition_point(notes.begin(), notes.end(), [start_tick](const midi::Note& note) {
                return note.get_start() < start_tick;
            });

        for (auto it = partition; it != notes.end(); ++it) {
            const midi::Note& note = *it;

            {
                std::lock_guard lock(events_mutex_);
                if (note.velocity >= 20) {
                    events_[note.get_start()].push_back(
                        NoteOnEv{note.channel, note.key, note.velocity});
                    events_[note.end()].push_back(NoteOffEv{note.channel, note.key, note.velocity});
                }

                if (events_.size() > 65536) {
                    Debugger::log(
                        "Event generation is producing too much events, throttling for 100ms...");
                }
            }

            if (events_.size() > 65536) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }

            if (reset_requested_.load(std::memory_order_relaxed)) {
                break;
            }
        }

        for (const midi::ChannelEvent& ch_ev : ch_evs) {
            {
                std::lock_guard lock(events_mutex_);

                if (const auto* controller = std::get_if<midi::Controller>(&ch_ev.event_type)) {
                    events_[ch_ev.tick].push_back(
                        ControlEv{ch_ev.channel, controller->controller, controller->value});
                } else if (const auto* bend = std::get_if<midi::PitchBend>(&ch_ev.event_type)) {
                    events_[ch_ev.tick].push_back(PitchBendEv{ch_ev.channel, bend->lsb, bend->msb});
                }
            }

            if (reset_requested_.load(std::memory_order_relaxed)) {
                break;
            }
        }

        if (reset_requested_.load(std::memory_order_relaxed)) {
            break;
        }
    }

    {
        std::lock_guard lock(events_mutex_);
        Debugger::log(
            std::format("Generated {} scheduled events for prerender engine.", events_.size()));
    }
}

void PrerenderEngine::render_audio(float start_time, MIDITick, float speed) {
    constexpr std::size_t buff_len = AUDIO_BUFFER_SIZE;

    const auto get_skipping_velocity = [](std::size_t wr, std::size_t rd) -> std::uint8_t {
        const std::size_t diff = (wr > rd ? wr - rd : 0) / 100;
        const int v = 137 - static_cast<int>(diff);
        return static_cast<std::uint8_t>(std::clamp(v, 0, 127));
    };

    const auto write_wrapped = [this](std::size_t start, std::size_t count) {
        std::lock_guard lock(audio_buffer_mutex_);

        start = (start * 2) % buff_len;
        std::size_t samples = count * 2;
        if (start + samples > buff_len) {
            const std::size_t first = buff_len - start;
            synth_->read_samples(audio_buffer_.data() + start, first / 2);
            samples -= first;
            synth_->read_samples(audio_buffer_.data(), samples / 2);
        } else {
            synth_->read_samples(audio_buffer_.data() + start, samples / 2);
        }
    };

    read_pos_.store(0, std::memory_order_relaxed);
    write_pos_.store(0, std::memory_order_relaxed);

    while (true) {
        bool can_process_events = true;

        MIDITick ev_tick_time = 0;
        std::vector<MidiEvent> evs;
        {
            std::unique_lock lock(events_mutex_);
            if (events_.empty()) {
                lock.unlock();
                if (reset_requested_.load(std::memory_order_relaxed)) {
                    break;
                }
                can_process_events = false;
            } else {
                const auto first = events_.begin();
                ev_tick_time = first->first;
                evs = std::move(first->second);
                events_.erase(first);
            }
        }

        if (reset_requested_.load(std::memory_order_relaxed)) {
            break;
        }

        if (can_process_events) {
            for (const MidiEvent& ev : evs) {
                if (write_pos_.load(std::memory_order_relaxed) <
                    read_pos_.load(std::memory_order_relaxed)) {
                    write_pos_.store(read_pos_.load(std::memory_order_relaxed),
                                     std::memory_order_relaxed);
                }

                const float ev_time = [&]() {
                    const float t = tempo_map_->ticks_to_secs_from_map(
                        ppq_.load(std::memory_order_relaxed), ev_tick_time);
                    return t / speed;
                }();

                const float offset = ev_time - start_time;
                const auto samples_signed =
                    static_cast<std::ptrdiff_t>(offset * static_cast<float>(AUDIO_SAMPLERATE)) -
                    static_cast<std::ptrdiff_t>(write_pos_.load(std::memory_order_relaxed));

                if (samples_signed > 0) {
                    auto samples = static_cast<std::size_t>(samples_signed);
                    while (write_pos_.load(std::memory_order_relaxed) + samples >
                           read_pos_.load(std::memory_order_relaxed) + AUDIO_BUFFER_SIZE / 2) {
                        std::size_t spare = (read_pos_.load(std::memory_order_relaxed) +
                                             AUDIO_BUFFER_SIZE / 2) -
                                            write_pos_.load(std::memory_order_relaxed);
                        if (spare > 0) {
                            if (spare > samples) {
                                spare = samples;
                            }
                            if (spare != 0) {
                                write_wrapped(write_pos_.load(std::memory_order_relaxed), spare);
                                samples -= spare;
                                write_pos_.fetch_add(spare, std::memory_order_relaxed);
                            }
                            if (samples == 0) {
                                break;
                            }
                        }

                        std::this_thread::sleep_for(std::chrono::milliseconds(2));
                        if (reset_requested_.load(std::memory_order_relaxed)) {
                            break;
                        }
                    }

                    if (samples != 0) {
                        write_wrapped(write_pos_.load(std::memory_order_relaxed), samples);
                    }

                    write_pos_.fetch_add(samples, std::memory_order_relaxed);
                }

                const std::size_t wr = write_pos_.load(std::memory_order_relaxed);
                const std::size_t rd = read_pos_.load(std::memory_order_relaxed);

                std::visit(
                    [&](const auto& e) {
                        using T = std::decay_t<decltype(e)>;

                        if constexpr (std::is_same_v<T, NoteOnEv>) {
                            if (e.velocity < get_skipping_velocity(wr, rd)) {
                                return;
                            }
                            synth_->note_on(e.channel, e.key, e.velocity);
                        } else if constexpr (std::is_same_v<T, NoteOffEv>) {
                            if (e.velocity < get_skipping_velocity(wr, rd)) {
                                return;
                            }
                            synth_->note_off(e.channel, e.key);
                        } else if constexpr (std::is_same_v<T, ControlEv>) {
                            synth_->control(e.channel, e.controller, e.value);
                        } else {
                            static_assert(std::is_same_v<T, PitchBendEv>);
                            synth_->pitch_bend(e.channel,
                                               (static_cast<int>(e.msb) << 7) |
                                                   static_cast<int>(e.lsb));
                        }
                    },
                    ev);

                if (reset_requested_.load(std::memory_order_relaxed)) {
                    break;
                }
            }
        }

        if (reset_requested_.load(std::memory_order_relaxed)) {
            break;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }

    while (!reset_requested_.load(std::memory_order_relaxed)) {
        const std::size_t rd = read_pos_.load(std::memory_order_relaxed);
        const std::size_t wr = write_pos_.load(std::memory_order_relaxed);
        if (rd + AUDIO_BUFFER_SIZE / 2 > wr) {
            const std::size_t spare = rd + AUDIO_BUFFER_SIZE / 2 - wr;
            write_wrapped(wr, spare);
            write_pos_.fetch_add(spare, std::memory_order_relaxed);
        } else {
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
    }

    synth_->all_notes_killed();
    synth_->reset_control();

    {
        std::lock_guard lock(events_mutex_);
        events_.clear();
    }
}

float PrerenderEngine::get_player_time() const {
    const std::size_t read_pos = read_pos_.load(std::memory_order_relaxed);
    return start_time_ + static_cast<float>(read_pos) / static_cast<float>(AUDIO_SAMPLERATE);
}

float PrerenderEngine::get_buffer_seconds() const {
    const auto read_pos = static_cast<float>(read_pos_.load(std::memory_order_relaxed));
    const auto write_pos = static_cast<float>(write_pos_.load(std::memory_order_relaxed));
    return std::max(0.0f, write_pos - read_pos) / static_cast<float>(AUDIO_SAMPLERATE);
}

void PrerenderEngine::start_prerender(float start_time, MIDITick start_tick, float speed) {
    kill_last_generators();
    start_time_ = start_time / speed;
    reset_requested_.store(false, std::memory_order_relaxed);

    generator_thread_ =
        std::thread([this, start_time, start_tick]() { generate_events(start_time, start_tick); });
    event_generator_thread_ = std::thread(
        [this, start_time, start_tick, speed]() { render_audio(start_time, start_tick, speed); });
}

void PrerenderEngine::stop_prerender() {
    kill_last_generators();
    reset_requested_.store(false, std::memory_order_relaxed);
    read_pos_.store(0, std::memory_order_relaxed);
    write_pos_.store(0, std::memory_order_relaxed);
}

void PrerenderEngine::sync_player(float start_time, float speed) {
    const auto read_pos = read_pos_.load(std::memory_order_relaxed);
    const float time = start_time / speed;
    const float t = start_time_ + static_cast<float>(read_pos) / static_cast<float>(AUDIO_SAMPLERATE);
    const float offs = time - t;

    auto new_pos = static_cast<std::ptrdiff_t>(read_pos) +
                   static_cast<std::ptrdiff_t>(offs * static_cast<float>(AUDIO_SAMPLERATE));
    if (new_pos < 0) {
        new_pos = 0;
    }

    if (std::abs(static_cast<float>(static_cast<std::ptrdiff_t>(read_pos) - new_pos)) /
            static_cast<float>(AUDIO_SAMPLERATE) >
        0.03f) {
        read_pos_.store(static_cast<std::size_t>(new_pos), std::memory_order_relaxed);
    }
}

void PrerenderEngine::kill_last_generators() {
    reset_requested_.store(true, std::memory_order_relaxed);

    if (generator_thread_.joinable()) {
        generator_thread_.join();
    }
    if (event_generator_thread_.joinable()) {
        event_generator_thread_.join();
    }

    std::lock_guard lock(audio_buffer_mutex_);
    std::fill(audio_buffer_.begin(), audio_buffer_.end(), 0.0f);
}

void PrerenderEngine::start_audio(MIDITick time, float speed, bool force) {
    const std::uint16_t ppq = ppq_.load(std::memory_order_relaxed);

    const float time_secs = [&]() {
        return tempo_map_->ticks_to_secs_from_map(ppq, time);
    }();

    const float player_time = get_player_time();
    if (time_secs + 0.1f > player_time || time_secs + 0.01f < player_time) {
        force = true;
    }

    if (force) {
        start_prerender(time_secs, time, speed);
    } else {
        sync_player(time_secs, speed);
    }
}

#ifdef _WIN32

bool PrerenderEngine::open_stream() {
    stream_ = std::make_unique<Stream>();

    WAVEFORMATEX format{};
    format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    format.nChannels = static_cast<WORD>(AUDIO_CHANNELS);
    format.nSamplesPerSec = static_cast<DWORD>(AUDIO_SAMPLERATE);
    format.wBitsPerSample = 32;
    format.nBlockAlign = static_cast<WORD>(AUDIO_CHANNELS * sizeof(float));
    format.nAvgBytesPerSec = format.nSamplesPerSec * format.nBlockAlign;
    format.cbSize = 0;

    stream_->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (stream_->event == nullptr) {
        Debugger::log_error("Prerender engine could not create its output event.");
        stream_.reset();
        return false;
    }

    const MMRESULT result =
        waveOutOpen(&stream_->handle, WAVE_MAPPER, &format,
                    reinterpret_cast<DWORD_PTR>(stream_->event), 0, CALLBACK_EVENT);
    if (result != MMSYSERR_NOERROR) {
        Debugger::log_error(
            std::format("Error starting prerender engine stream: waveOutOpen {}", result));
        CloseHandle(stream_->event);
        stream_.reset();
        return false;
    }

    for (std::size_t i = 0; i < OUTPUT_BUFFERS; ++i) {
        stream_->buffers[i].assign(OUTPUT_FRAMES * AUDIO_CHANNELS, 0.0f);
        WAVEHDR& header = stream_->headers[i];
        header = {};
        header.lpData = reinterpret_cast<LPSTR>(stream_->buffers[i].data());
        header.dwBufferLength =
            static_cast<DWORD>(stream_->buffers[i].size() * sizeof(float));
        waveOutPrepareHeader(stream_->handle, &header, sizeof(WAVEHDR));
        header.dwFlags |= WHDR_DONE;
    }

    output_running_.store(true, std::memory_order_relaxed);
    output_thread_ = std::thread([this]() { output_thread_main(); });

    return true;
}

void PrerenderEngine::output_thread_main() {
    while (output_running_.load(std::memory_order_relaxed)) {
        bool queued_any = false;

        for (std::size_t i = 0; i < OUTPUT_BUFFERS; ++i) {
            WAVEHDR& header = stream_->headers[i];
            if ((header.dwFlags & WHDR_DONE) == 0) {
                continue;
            }

            fill_output(stream_->buffers[i].data(), stream_->buffers[i].size());

            header.dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(stream_->handle, &header, sizeof(WAVEHDR)) != MMSYSERR_NOERROR) {
                header.dwFlags |= WHDR_DONE;
            } else {
                queued_any = true;
            }
        }

        if (!queued_any) {
            WaitForSingleObject(stream_->event, 20);
        }
    }
}

void PrerenderEngine::close_stream() {
    if (!stream_) {
        return;
    }

    output_running_.store(false, std::memory_order_relaxed);
    if (stream_->event != nullptr) {
        SetEvent(stream_->event);
    }
    if (output_thread_.joinable()) {
        output_thread_.join();
    }

    if (stream_->handle != nullptr) {
        waveOutReset(stream_->handle);
        for (WAVEHDR& header : stream_->headers) {
            waveOutUnprepareHeader(stream_->handle, &header, sizeof(WAVEHDR));
        }
        waveOutClose(stream_->handle);
    }

    if (stream_->event != nullptr) {
        CloseHandle(stream_->event);
    }

    stream_.reset();
}

#else

bool PrerenderEngine::open_stream() { return false; }
void PrerenderEngine::output_thread_main() {}
void PrerenderEngine::close_stream() {}

#endif

}
