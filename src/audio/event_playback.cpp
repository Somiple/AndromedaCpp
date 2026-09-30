#include "audio/event_playback.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <format>
#include <limits>
#include <stdexcept>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <windows.h>

#include <timeapi.h>
#endif

#include "util/debugger.h"

namespace andromeda::audio {

using util::Debugger;

std::optional<int> compare_time(const MIDITimeType& a, const MIDITimeType& b) {
    if (std::holds_alternative<TicksTime>(a) && std::holds_alternative<TicksTime>(b)) {
        const MIDITick x = std::get<TicksTime>(a).value;
        const MIDITick y = std::get<TicksTime>(b).value;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    if (std::holds_alternative<SecondsTime>(a) && std::holds_alternative<SecondsTime>(b)) {
        const float x = std::get<SecondsTime>(a).value;
        const float y = std::get<SecondsTime>(b).value;
        return x < y ? -1 : (x > y ? 1 : 0);
    }
    return std::nullopt;
}

namespace {

bool time_le(const MIDITimeType& a, const MIDITimeType& b) {
    const auto ord = compare_time(a, b);
    return ord.has_value() && *ord <= 0;
}

std::atomic<int> g_live_threads{0};

struct LiveThread {
    LiveThread() { g_live_threads.fetch_add(1, std::memory_order_relaxed); }
    ~LiveThread() { g_live_threads.fetch_sub(1, std::memory_order_relaxed); }
    LiveThread(const LiveThread&) = delete;
    LiveThread& operator=(const LiveThread&) = delete;
};

class PlaybackClock {
public:
    PlaybackClock() {
#ifdef _WIN32
        timer_ = CreateWaitableTimerExW(nullptr, nullptr,
                                        CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        raised_period_ = timeBeginPeriod(1) == TIMERR_NOERROR;
#endif
    }

    ~PlaybackClock() {
#ifdef _WIN32
        if (timer_ != nullptr) {
            CloseHandle(timer_);
        }
        if (raised_period_) {
            timeEndPeriod(1);
        }
#endif
    }

    PlaybackClock(const PlaybackClock&) = delete;
    PlaybackClock& operator=(const PlaybackClock&) = delete;

    void sleep_one_ms() const {
#ifdef _WIN32
        if (timer_ != nullptr) {
            LARGE_INTEGER due;
            due.QuadPart = -10000;
            if (SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE) != 0) {
                WaitForSingleObject(timer_, INFINITE);
                return;
            }
        }
#endif
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

private:
#ifdef _WIN32
    HANDLE timer_ = nullptr;
    bool raised_period_ = false;
#endif
};

class AudioThreadPriority {
public:
    AudioThreadPriority() {
#ifdef _WIN32
        avrt_ = LoadLibraryW(L"avrt.dll");
        if (avrt_ != nullptr) {
            using SetFn = HANDLE(WINAPI*)(LPCWSTR, LPDWORD);
            auto set = reinterpret_cast<SetFn>(
                reinterpret_cast<void*>(GetProcAddress(avrt_, "AvSetMmThreadCharacteristicsW")));
            if (set != nullptr) {
                DWORD index = 0;
                task_ = set(L"Pro Audio", &index);
            }
        }
        if (task_ == nullptr) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        }
#endif
    }

    ~AudioThreadPriority() {
#ifdef _WIN32
        if (task_ != nullptr && avrt_ != nullptr) {
            using RevertFn = BOOL(WINAPI*)(HANDLE);
            auto revert = reinterpret_cast<RevertFn>(
                reinterpret_cast<void*>(GetProcAddress(avrt_, "AvRevertMmThreadCharacteristics")));
            if (revert != nullptr) {
                revert(task_);
            }
        }
        if (avrt_ != nullptr) {
            FreeLibrary(avrt_);
        }
#endif
    }

    AudioThreadPriority(const AudioThreadPriority&) = delete;
    AudioThreadPriority& operator=(const AudioThreadPriority&) = delete;

private:
#ifdef _WIN32
    HMODULE avrt_ = nullptr;
    HANDLE task_ = nullptr;
#endif
};

constexpr int KEY_SLOTS = 128;
constexpr int CHANNELS = 16;

constexpr editor::MIDITick UNENDED = std::numeric_limits<editor::MIDITick>::max();

editor::MIDITick note_end(const midi::Note& note) {
    if (note.length == UNENDED || note.length > UNENDED - note.start) {
        return UNENDED;
    }
    return note.start + note.length;
}

std::uint32_t order_key(bool art_like, std::uint16_t track_rank, std::uint8_t velocity,
                        editor::MIDITick duration) {
    const std::uint32_t art = art_like ? 1u : 0u;
    const std::uint32_t rank = std::min<std::uint32_t>(track_rank, 0x7FF);
    const std::uint32_t vel = 127u - std::min<std::uint32_t>(velocity, 127u);
    const std::uint32_t dur = 0xFFFu - std::min<std::uint32_t>(duration, 0xFFFu);
    return (art << 31) | (rank << 20) | (vel << 13) | dur;
}

std::uint64_t tracks_signature(const std::vector<midi::MIDITrack>& tracks) {
    std::uint64_t sig = 0x9E3779B97F4A7C15ull * (tracks.size() + 1);
    for (const midi::MIDITrack& track : tracks) {
        sig ^= track.revision + 0x9E3779B97F4A7C15ull + (sig << 6) + (sig >> 2);
        sig ^= track.get_notes().size() + 0x165667B19E3779F9ull + (sig << 6) + (sig >> 2);
    }
    return sig;
}

struct PendingNote {
    std::uint64_t order;
    editor::MIDITick end;
    std::uint8_t key;
    std::uint8_t channel;
    std::uint8_t velocity;
    bool short_drum;
};

class PlaybackRun {
public:
    PlaybackRun(std::shared_ptr<const NotePriorities> priorities, std::size_t track_count)
        : priorities_(std::move(priorities)),
          ch_cursors_(track_count, 0),
          ch_next_tick_(track_count, 0),
          note_cursors_(track_count, 0),
          next_start_(track_count, UNENDED),
          revisions_(track_count, 0),
          muted_(track_count, 0) {
        pending_.reserve(4096);
        out_.reserve(4096);

        if (const char* env = std::getenv("ANDROMEDA_VOICE_BUDGET")) {
            const long value = std::strtol(env, nullptr, 10);
            voice_budget_ = value > 0 ? static_cast<std::size_t>(value)
                                      : std::numeric_limits<std::size_t>::max();
        }
    }

    [[nodiscard]] std::size_t passes_over_budget() const { return over_budget_; }

    void reset_to(MIDITick tick, const std::vector<midi::MIDITrack>& tracks) {

        for (int key = 0; key < KEY_SLOTS; ++key) {
            for (int ch = 0; ch < CHANNELS; ++ch) {
                active_ends_[key][ch].clear();
            }
        }
        std::fill(soonest_end_.begin(), soonest_end_.end(),
                  std::numeric_limits<editor::MIDITick>::max());

        seen_edits_ = midi::g_track_revisions.load(std::memory_order_acquire);
        soonest_next_ = UNENDED;
        for (std::size_t t = 0; t < note_cursors_.size() && t < tracks.size(); ++t) {
            seek_track(t, tracks[t], tick);
            soonest_next_ = std::min(soonest_next_, next_start_[t]);
        }

        for (std::size_t t = 0; t < ch_cursors_.size() && t < tracks.size(); ++t) {
            const auto& evs = tracks[t].get_channel_evs();
            ch_cursors_[t] = static_cast<std::size_t>(
                std::partition_point(evs.begin(), evs.end(),
                                     [tick](const midi::ChannelEvent& e) { return e.tick < tick; }) -
                evs.begin());
        }
        ch_passes_ = 0;

        last_tick_ = tick;
        // nothing at the start tick has been played yet; notes there were skipped before
        include_last_tick_ = true;
    }

    void set_muted(std::size_t track, bool muted) {
        if (track < muted_.size()) {
            muted_[track] = muted ? 1 : 0;
        }
    }

    struct PassCost {
        double offs = 0.0;
        double gather = 0.0;
        double sort = 0.0;
        double send = 0.0;
        std::size_t note_ons = 0;
        std::size_t events = 0;
    };

    void update_tick(MIDIAudioEngine& dev, MIDITick now,
                     const std::vector<midi::MIDITrack>& tracks, PassCost* cost = nullptr) {
        const auto stamp = [] { return std::chrono::steady_clock::now(); };
        const auto us = [](auto a, auto b) {
            return std::chrono::duration<double, std::micro>(b - a).count();
        };

        out_.clear();

        const auto t0 = stamp();
        send_channel_events(now, tracks);
        send_due_note_offs(now);
        const auto t1 = stamp();
        gather_note_ons(now, tracks);
        const auto t2 = stamp();
        order_note_ons();
        const auto t3 = stamp();
        send_note_ons();

        if (!out_.empty()) {
            dev.send_events(out_);
        }
        const auto t4 = stamp();

        if (cost != nullptr) {
            cost->offs += us(t0, t1);
            cost->gather += us(t1, t2);
            cost->sort += us(t2, t3);
            cost->send += us(t3, t4);
            cost->note_ons += pending_.size();
            cost->events += out_.size();
        }

        last_tick_ = now;
        include_last_tick_ = false;
    }

    void seek_to(MIDIAudioEngine& dev, MIDITick tick,
                 const std::vector<midi::MIDITrack>& tracks) {
        all_notes_off(dev);
        reset_midi_state(dev);
        reset_to(tick, tracks);
        replay_channel_state(dev, tracks);
    }

    void all_notes_off(MIDIAudioEngine& dev) {
        out_.clear();
        for (int key = 0; key < KEY_SLOTS; ++key) {
            for (int ch = 0; ch < CHANNELS; ++ch) {
                for (std::size_t i = 0; i < active_ends_[key][ch].size(); ++i) {
                    out_.push_back({static_cast<std::uint8_t>(0x80 | ch),
                                    static_cast<std::uint8_t>(key), 0});
                }
                active_ends_[key][ch].clear();
            }
        }
        std::fill(soonest_end_.begin(), soonest_end_.end(),
                  std::numeric_limits<editor::MIDITick>::max());

        for (int ch = 0; ch < CHANNELS; ++ch) {
            out_.push_back({static_cast<std::uint8_t>(0xB0 | ch), 123, 0});
            out_.push_back({static_cast<std::uint8_t>(0xB0 | ch), 120, 0});
        }

        dev.send_events(out_);
        out_.clear();
    }

private:
    void reset_midi_state(MIDIAudioEngine& dev) {
        out_.clear();
        for (int ch = 0; ch < CHANNELS; ++ch) {
            const auto c = static_cast<std::uint8_t>(0xB0 | ch);
            out_.push_back({c, 120, 0});
            out_.push_back({c, 123, 0});
            out_.push_back({c, 121, 0});
            out_.push_back({c, 1, 0});
            out_.push_back({c, 7, 100});
            out_.push_back({c, 10, 64});
            out_.push_back({c, 11, 127});
            out_.push_back({c, 64, 0});
            out_.push_back({c, 65, 0});
            out_.push_back({c, 66, 0});
            out_.push_back({c, 67, 0});
            out_.push_back({c, 98, 127});
            out_.push_back({c, 99, 127});
            out_.push_back({c, 100, 127});
            out_.push_back({c, 101, 127});
            out_.push_back({static_cast<std::uint8_t>(0xE0 | ch), 0, 64});
            out_.push_back({static_cast<std::uint8_t>(0xC0 | ch), 0, 0});
        }
        dev.send_events(out_);
        out_.clear();
    }

    void replay_channel_state(MIDIAudioEngine& dev,
                              const std::vector<midi::MIDITrack>& tracks) {
        std::array<std::array<std::uint8_t, 128>, CHANNELS> cc{};
        std::array<std::array<bool, 128>, CHANNELS> cc_set{};
        std::array<std::uint8_t, CHANNELS> program{};
        std::array<bool, CHANNELS> program_set{};
        std::array<std::pair<std::uint8_t, std::uint8_t>, CHANNELS> bend{};
        std::array<bool, CHANNELS> bend_set{};

        for (std::size_t t = 0; t < ch_cursors_.size() && t < tracks.size(); ++t) {
            if (muted_[t] != 0) {
                continue;
            }
            const auto& evs = tracks[t].get_channel_evs();
            const std::size_t end = std::min(ch_cursors_[t], evs.size());
            for (std::size_t i = 0; i < end; ++i) {
                const midi::ChannelEvent& ev = evs[i];
                const std::size_t ch = ev.channel & 0x0F;
                if (const auto* ctrl = std::get_if<midi::Controller>(&ev.event_type)) {
                    cc[ch][ctrl->controller & 0x7F] = ctrl->value;
                    cc_set[ch][ctrl->controller & 0x7F] = true;
                } else if (const auto* prog = std::get_if<midi::ProgramChange>(&ev.event_type)) {
                    program[ch] = prog->program;
                    program_set[ch] = true;
                } else if (const auto* pb = std::get_if<midi::PitchBend>(&ev.event_type)) {
                    bend[ch] = {pb->lsb, pb->msb};
                    bend_set[ch] = true;
                }
            }
        }

        out_.clear();
        for (std::size_t ch = 0; ch < CHANNELS; ++ch) {
            if (program_set[ch]) {
                out_.push_back({static_cast<std::uint8_t>(0xC0 | ch), program[ch], 0});
            }
            for (std::size_t c = 0; c < 128; ++c) {
                if (cc_set[ch][c]) {
                    out_.push_back({static_cast<std::uint8_t>(0xB0 | ch),
                                    static_cast<std::uint8_t>(c), cc[ch][c]});
                }
            }
            if (bend_set[ch]) {
                out_.push_back(
                    {static_cast<std::uint8_t>(0xE0 | ch), bend[ch].first, bend[ch].second});
            }
        }
        dev.send_events(out_);
        out_.clear();
    }

    static std::optional<std::array<std::uint8_t, 3>> channel_message(
        const midi::ChannelEvent& ev) {
        if (const auto* ctrl = std::get_if<midi::Controller>(&ev.event_type)) {
            return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(0xB0 | ev.channel),
                                               ctrl->controller, ctrl->value};
        }
        if (const auto* bend = std::get_if<midi::PitchBend>(&ev.event_type)) {
            return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(0xE0 | ev.channel),
                                               bend->lsb, bend->msb};
        }
        if (const auto* prog = std::get_if<midi::ProgramChange>(&ev.event_type)) {
            return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(0xC0 | ev.channel),
                                               prog->program, 0};
        }
        if (const auto* touch = std::get_if<midi::ChannelAftertouch>(&ev.event_type)) {
            return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(0xD0 | ev.channel),
                                               touch->amount, 0};
        }
        if (const auto* touch = std::get_if<midi::NoteAftertouch>(&ev.event_type)) {
            return std::array<std::uint8_t, 3>{static_cast<std::uint8_t>(0xA0 | ev.channel),
                                               touch->key, touch->pressure};
        }
        return std::nullopt;
    }

    void send_channel_events(MIDITick now, const std::vector<midi::MIDITrack>& tracks) {
        // full rescan every 64 passes: channel event edits do not bump the revision
        const bool refresh = (ch_passes_++ & 63u) == 0;
        if (!refresh && ch_soonest_ > now) {
            return;
        }

        editor::MIDITick soonest = UNENDED;
        const std::size_t count = std::min(ch_cursors_.size(), tracks.size());
        for (std::size_t t = 0; t < count; ++t) {
            if (!refresh && ch_next_tick_[t] > now) {
                soonest = std::min(soonest, ch_next_tick_[t]);
                continue;
            }

            const auto& evs = tracks[t].get_channel_evs();
            std::size_t cursor = ch_cursors_[t];
            if (muted_[t] == 0) {
                while (cursor < evs.size() && evs[cursor].tick <= now) {
                    if (const auto msg = channel_message(evs[cursor])) {
                        out_.push_back(*msg);
                    }
                    cursor += 1;
                }
                ch_cursors_[t] = cursor;
            }
            ch_next_tick_[t] = cursor < evs.size() ? evs[cursor].tick : UNENDED;
            soonest = std::min(soonest, ch_next_tick_[t]);
        }
        ch_soonest_ = soonest;
    }

    void send_due_note_offs(MIDITick now) {
        for (std::size_t slot = 0; slot < KEY_SLOTS * CHANNELS; ++slot) {
            if (soonest_end_[slot] > now) {
                continue;
            }

            const int key = static_cast<int>(slot / CHANNELS);
            const int ch = static_cast<int>(slot % CHANNELS);
            std::vector<editor::MIDITick>& ends = active_ends_[key][ch];

            std::size_t write = 0;
            std::size_t expired = 0;
            editor::MIDITick soonest = std::numeric_limits<editor::MIDITick>::max();
            for (std::size_t e = 0; e < ends.size(); ++e) {
                if (ends[e] <= now) {
                    expired += 1;
                } else {
                    soonest = std::min(soonest, ends[e]);
                    ends[write++] = ends[e];
                }
            }
            ends.resize(write);
            soonest_end_[slot] = soonest;

            // one note-off per expired note: omnimidi takes a voice per note-on
            for (std::size_t e = 0; e < expired; ++e) {
                out_.push_back(
                    {static_cast<std::uint8_t>(0x80 | ch), static_cast<std::uint8_t>(key), 0});
            }
        }
    }

    void seek_track(std::size_t t, const midi::MIDITrack& track, MIDITick tick) {
        const std::vector<midi::Note>& notes = track.get_notes();
        const std::size_t pos = static_cast<std::size_t>(
            std::partition_point(notes.begin(), notes.end(),
                                 [tick](const midi::Note& n) { return n.start < tick; }) -
            notes.begin());
        note_cursors_[t] = pos;
        next_start_[t] = pos < notes.size() ? notes[pos].start : UNENDED;
        revisions_[t] = track.revision;
    }

    void gather_note_ons(MIDITick now, const std::vector<midi::MIDITrack>& tracks) {
        pending_.clear();

        const std::uint64_t edits = midi::g_track_revisions.load(std::memory_order_acquire);
        const bool check_revisions = edits != seen_edits_;
        seen_edits_ = edits;

        if (!check_revisions && soonest_next_ > now) {
            return;
        }

        const NotePriorities& prio = *priorities_;
        const std::size_t count = std::min(note_cursors_.size(), tracks.size());
        editor::MIDITick soonest = UNENDED;

        for (std::size_t t = 0; t < count; ++t) {
            if (check_revisions && tracks[t].revision != revisions_[t]) {
                seek_track(t, tracks[t], last_tick_);
            }
            if (next_start_[t] > now) {
                soonest = std::min(soonest, next_start_[t]);
                continue;
            }

            const midi::MIDITrack& track = tracks[t];

            const std::vector<midi::Note>& notes = track.get_notes();
            const std::size_t size = notes.size();
            const bool muted = muted_[t] != 0;
            const bool art = t < prio.art_track.size() && prio.art_track[t];
            const std::uint16_t rank = t < prio.track_rank.size() ? prio.track_rank[t] : 0;

            std::size_t pos = note_cursors_[t];
            while (pos < size && notes[pos].start <= now) {
                const midi::Note& note = notes[pos];
                pos += 1;

                const bool played = note.start < last_tick_ ||
                                    (note.start == last_tick_ && !include_last_tick_);
                if (played || muted) {
                    continue;
                }
                if (note.velocity < 20) {
                    continue;
                }

                const editor::MIDITick end = note_end(note);
                const bool already_over = end <= now;
                const auto channel = static_cast<std::uint8_t>(note.channel & 0x0F);
                if (already_over && channel != 9) {
                    continue;
                }

                const editor::MIDITick duration = end > note.start ? end - note.start : 0;
                const std::uint32_t priority = order_key(
                    art && duration <= prio.short_ticks, rank, note.velocity, duration);

                pending_.push_back(PendingNote{
                    .order = (static_cast<std::uint64_t>(note.start) << 32) | priority,
                    .end = end,
                    .key = static_cast<std::uint8_t>(note.key & 0x7F),
                    .channel = channel,
                    .velocity = note.velocity,
                    .short_drum = already_over});
            }

            note_cursors_[t] = pos;
            next_start_[t] = pos < size ? notes[pos].start : UNENDED;
            soonest = std::min(soonest, next_start_[t]);
        }

        soonest_next_ = soonest;
    }

    void order_note_ons() {
        if (pending_.size() > 1) {
            std::sort(pending_.begin(), pending_.end(),
                      [](const PendingNote& a, const PendingNote& b) { return a.order < b.order; });
        }

        if (pending_.size() > voice_budget_) {
            pending_.resize(voice_budget_);
            over_budget_ += 1;
        }
    }

    void send_note_ons() {
        for (const PendingNote& note : pending_) {
            if (note.short_drum) {
                const bool overlaps = !active_ends_[note.key][note.channel].empty();
                out_.push_back(
                    {static_cast<std::uint8_t>(0x90 | note.channel), note.key, note.velocity});
                if (!overlaps) {
                    out_.push_back({static_cast<std::uint8_t>(0x80 | note.channel), note.key, 0});
                }
                continue;
            }

            out_.push_back(
                {static_cast<std::uint8_t>(0x90 | note.channel), note.key, note.velocity});

            active_ends_[note.key][note.channel].push_back(note.end);

            const std::size_t slot = static_cast<std::size_t>(note.key) * CHANNELS + note.channel;
            soonest_end_[slot] = std::min(soonest_end_[slot], note.end);
        }
    }

    std::shared_ptr<const NotePriorities> priorities_;

    std::vector<editor::MIDITick> active_ends_[KEY_SLOTS][CHANNELS];
    std::vector<editor::MIDITick> soonest_end_ = std::vector<editor::MIDITick>(
        KEY_SLOTS * CHANNELS, std::numeric_limits<editor::MIDITick>::max());

    std::vector<std::size_t> ch_cursors_;
    std::vector<editor::MIDITick> ch_next_tick_;
    editor::MIDITick ch_soonest_ = 0;
    std::uint32_t ch_passes_ = 0;
    std::vector<std::size_t> note_cursors_;
    std::vector<editor::MIDITick> next_start_;
    std::vector<std::uint64_t> revisions_;
    std::vector<std::uint8_t> muted_;
    editor::MIDITick soonest_next_ = UNENDED;
    std::uint64_t seen_edits_ = 0;

    MIDITick last_tick_ = 0;
    bool include_last_tick_ = false;

    std::size_t voice_budget_ = 2048;
    std::size_t over_budget_ = 0;

    std::vector<PendingNote> pending_;
    std::vector<std::array<std::uint8_t, 3>> out_;
};

}

bool ScheduledSequence::earlier(const Scheduled& a, const Scheduled& b) {
    return time_le(a.get_time(), b.get_time());
}

void ScheduledSequence::sift_up(std::size_t index) {
    while (index > 0) {
        const std::size_t parent = (index - 1) / 2;
        if (!earlier(sequence[index], sequence[parent])) {
            break;
        }
        std::swap(sequence[index], sequence[parent]);
        index = parent;
    }
}

void ScheduledSequence::sift_down(std::size_t index) {
    const std::size_t n = sequence.size();
    for (;;) {
        const std::size_t left = index * 2 + 1;
        if (left >= n) {
            break;
        }
        const std::size_t right = left + 1;

        std::size_t smallest = left;
        if (right < n && earlier(sequence[right], sequence[left])) {
            smallest = right;
        }
        if (!earlier(sequence[smallest], sequence[index])) {
            break;
        }

        std::swap(sequence[index], sequence[smallest]);
        index = smallest;
    }
}

void ScheduledSequence::insert(Scheduled scheduled_event) {
    sequence.push_back(std::move(scheduled_event));
    sift_up(sequence.size() - 1);
}

std::optional<Scheduled> ScheduledSequence::pop() {
    if (sequence.empty()) {
        return std::nullopt;
    }

    Scheduled earliest_scheduled = std::move(sequence.front());
    sequence.front() = std::move(sequence.back());
    sequence.pop_back();
    if (!sequence.empty()) {
        sift_down(0);
    }

    return earliest_scheduled;
}

int live_playback_threads() { return g_live_threads.load(std::memory_order_relaxed); }

NotePriorities NotePriorities::build(const std::vector<midi::MIDITrack>& tracks,
                                     std::uint16_t ppq) {
    NotePriorities out;
    out.short_ticks = ppq != 0 ? static_cast<editor::MIDITick>(ppq / 2) : 240;
    out.track_notes.assign(tracks.size(), 0);
    out.art_track.assign(tracks.size(), false);

    std::uint32_t max_notes = 0;
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const auto count = static_cast<std::uint32_t>(tracks[t].get_notes().size());
        out.track_notes[t] = count;
        max_notes = std::max(max_notes, count);
    }

    constexpr std::size_t RATIO_SAMPLE = 1u << 16;

    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const std::uint32_t count = out.track_notes[t];
        if (count == 0 || static_cast<std::uint64_t>(count) * 10 > max_notes) {
            continue;
        }

        const std::vector<midi::Note>& notes = tracks[t].get_notes();
        const std::size_t sampled = std::min<std::size_t>(notes.size(), RATIO_SAMPLE);
        std::size_t short_notes = 0;
        for (std::size_t i = 0; i < sampled; ++i) {
            if (notes[i].length <= out.short_ticks) {
                short_notes += 1;
            }
        }

        out.art_track[t] = short_notes * 100 >= sampled * 80;
    }

    {
        std::vector<std::uint32_t> order(tracks.size());
        for (std::uint32_t i = 0; i < order.size(); ++i) {
            order[i] = i;
        }
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            if (out.track_notes[a] != out.track_notes[b]) {
                return out.track_notes[a] < out.track_notes[b];
            }
            return a < b;
        });

        out.track_rank.assign(tracks.size(), 0);
        for (std::size_t r = 0; r < order.size(); ++r) {
            out.track_rank[order[r]] =
                static_cast<std::uint16_t>(std::min<std::size_t>(r, 0xFFFF));
        }
    }

    return out;
}

PlaybackManager::PlaybackManager(SharedDevice device_,
                                 util::SharedPtr<std::vector<midi::MIDITrack>> tracks_,
                                 editor::SharedMetaEvents meta_events_,
                                 util::SharedPtr<editor::TempoMap> tempo_map_in)
    : meta_events(std::move(meta_events_)),
      tracks(std::move(tracks_)),
      device(std::move(device_)),
      playback_pos_ticks(std::make_shared<MIDITickAtomic>(0)),
      stop_playback_(std::make_shared<std::atomic<bool>>(false)),
      tempo_map_(std::move(tempo_map_in)) {}

void PlaybackManager::on_event(const app::AndromedaEvent& event) {
    if (const auto* ppq_changed = std::get_if<app::PPQChanged>(&event)) {
        ppq = ppq_changed->new_ppq;
    }
}

void PlaybackManager::navigate_to(MIDITick tick_pos) {
    playback_start_pos = tick_pos;
    playback_pos_ticks->store(tick_pos, std::memory_order_relaxed);

    if (playing && seek_request_ != nullptr) {
        seek_request_->store(static_cast<std::int64_t>(tick_pos) + 1, std::memory_order_release);
    }
}

void PlaybackManager::stop() {
    // stopping returns to where playback started, so playing again starts there
    playback_pos_ticks->store(playback_start_pos, std::memory_order_relaxed);
    stop_playback_->store(true, std::memory_order_seq_cst);

    {
        std::lock_guard lock(transport_clock_->mutex);
        transport_clock_->value.running = false;
    }
}

MIDITick PlaybackManager::get_playback_ticks() const {
    if (!playing) {
        return playback_start_pos;
    }

    TransportClock clock;
    {
        std::lock_guard lock(transport_clock_->mutex);
        clock = transport_clock_->value;
    }

    if (!clock.running || clock.epoch_ns == 0) {
        return playback_pos_ticks->load(std::memory_order_relaxed);
    }

    const auto now = std::chrono::steady_clock::now().time_since_epoch().count();
    const double elapsed = static_cast<double>(now - clock.epoch_ns) / 1e9;
    const auto secs = static_cast<float>(clock.start_secs + elapsed);

    std::shared_lock lock(tempo_map_->mutex);
    return tempo_map_->value.secs_to_ticks_from_map(ppq, secs);
}

void PlaybackManager::start_play_at_mouse(std::uint8_t key, std::uint8_t channel,
                                          std::uint8_t velocity) {
    if (play_at_mouse_ || playing) {
        return;
    }

    {
        std::lock_guard lock(device->mutex);
        const std::array<std::uint8_t, 3> ev = {static_cast<std::uint8_t>(0x90 | channel), key,
                                                velocity};
        (void)device->value->send_event(ev);
    }
    play_at_mouse_ = true;
    mouse_last_key_ = key;
}

void PlaybackManager::update_play_at_mouse(std::uint8_t key, std::uint8_t channel,
                                           std::uint8_t velocity) {
    if (!play_at_mouse_ || playing) {
        return;
    }
    if (mouse_last_key_ == key) {
        return;
    }

    {
        std::lock_guard lock(device->mutex);
        const std::array<std::uint8_t, 3> off = {static_cast<std::uint8_t>(0x80 | channel),
                                                 mouse_last_key_, 0x00};
        const std::array<std::uint8_t, 3> on = {static_cast<std::uint8_t>(0x90 | channel), key,
                                                velocity};
        (void)device->value->send_event(off);
        (void)device->value->send_event(on);
    }

    mouse_last_key_ = key;
}

void PlaybackManager::stop_play_at_mouse(std::uint8_t key, std::uint8_t channel) {
    if (!play_at_mouse_ || playing) {
        return;
    }
    // fixed rust bug: released the key under the mouse, not the one sounding (stuck note)
    (void)key;

    {
        std::lock_guard lock(device->mutex);
        const std::array<std::uint8_t, 3> off = {static_cast<std::uint8_t>(0x80 | channel),
                                                 mouse_last_key_, 0x00};
        (void)device->value->send_event(off);
    }

    play_at_mouse_ = false;
}

void PlaybackManager::reset_events() {
    std::lock_guard lock(device->mutex);
    const std::array<std::uint8_t, 3> all_notes_off = {0xB0, 0x7B, 0x00};
    (void)device->value->send_event(all_notes_off);
}

std::shared_future<std::shared_ptr<const NotePriorities>> PlaybackManager::ensure_priorities(
    const std::vector<midi::MIDITrack>& trks, std::uint16_t ppq_copy, bool background) {
    const std::uint64_t sig = tracks_signature(trks);
    if (priorities_.valid() && priorities_signature_ == sig) {
        return priorities_;
    }

    auto promise = std::make_shared<std::promise<std::shared_ptr<const NotePriorities>>>();
    priorities_ = promise->get_future().share();
    priorities_signature_ = sig;

    auto tracks_ref = tracks;
    std::thread([tracks_ref, ppq_copy, promise, background]() {
#ifdef _WIN32
        if (background) {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_BELOW_NORMAL);
        }
#else
        (void)background;
#endif
        const auto build_start = std::chrono::steady_clock::now();
        std::shared_ptr<const NotePriorities> built;
        std::size_t track_count = 0;
        {
            std::shared_lock build_lock(tracks_ref->mutex);
            const auto& build_trks = tracks_ref->value;
            track_count = build_trks.size();
            built = std::make_shared<const NotePriorities>(
                NotePriorities::build(build_trks, ppq_copy));
        }
        const double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - build_start)
                .count();
        util::Debugger::log(
            std::format("Play order: {} tracks in {:.0f} ms", track_count, ms));
        promise->set_value(std::move(built));
    }).detach();

    return priorities_;
}

void PlaybackManager::prewarm_priorities() {
    std::shared_lock lock(tracks->mutex);
    (void)ensure_priorities(tracks->value, ppq, true);
}

void PlaybackManager::start_playback() {
    const std::uint16_t ppq_copy = ppq;

    // fresh stop flag per run, or a quick restart leaves two runs feeding the device
    auto stop_flag = std::make_shared<std::atomic<bool>>(false);
    stop_playback_ = stop_flag;

    auto seek_request = std::make_shared<std::atomic<std::int64_t>>(0);
    seek_request_ = seek_request;

    auto tracks_ref = tracks;
    auto playback_pos = playback_pos_ticks;
    auto device_ref = device;
    auto transport_clock = transport_clock_;

    editor::TempoMap tempo_copy;
    {
        std::shared_lock lock(tempo_map_->mutex);
        tempo_copy = tempo_map_->value;
    }

    // the shared position is written by a run that may still be finishing its last pass
    const MIDITick start_tick = playback_start_pos;
    playback_pos->store(start_tick, std::memory_order_relaxed);
    const float start_pos_secs = tempo_copy.ticks_to_secs_from_map(ppq_copy, start_tick);
    start_pos_secs_from_ticks_ = start_pos_secs;

    std::shared_future<std::shared_ptr<const NotePriorities>> priorities_future;
    {
        std::shared_lock lock(tracks_ref->mutex);
        priorities_future = ensure_priorities(tracks_ref->value, ppq_copy);
    }

    std::thread([tracks_ref, stop_flag, seek_request, playback_pos, device_ref, transport_clock,
                 tempo_copy, ppq_copy, start_tick, start_pos_secs, priorities_future]() {
        const LiveThread live;
        const AudioThreadPriority thread_priority;
        const PlaybackClock clock;

        const std::shared_ptr<const NotePriorities> priorities = priorities_future.get();

        std::size_t track_count = 0;
        {
            std::shared_lock lock(tracks_ref->mutex);
            track_count = tracks_ref->value.size();
        }

        PlaybackRun run(priorities, track_count);
        {
            std::shared_lock lock(tracks_ref->mutex);
            const auto& trks = tracks_ref->value;
            for (std::size_t t = 0; t < trks.size(); ++t) {
                run.set_muted(t, trks[t].muted);
            }
            std::lock_guard device_lock(device_ref->mutex);
            run.seek_to(*device_ref->value, start_tick, trks);
        }

        double music_start_secs = start_pos_secs;
        auto play_start = std::chrono::steady_clock::now();

        const auto publish_clock = [&]() {
            std::lock_guard lock(transport_clock->mutex);
            transport_clock->value = TransportClock{
                .epoch_ns = play_start.time_since_epoch().count(),
                .start_secs = music_start_secs,
                .running = true};
        };
        publish_clock();

        const bool probe_on = std::getenv("ANDROMEDA_PLAYBACK_PROBE") != nullptr;
        auto probe_since = play_start;
        std::size_t passes = 0;
        double total_pass_us = 0.0;
        double worst_pass_us = 0.0;
        MIDITick last_reported_tick = start_tick;
        MIDITick ticks_advanced = 0;
        PlaybackRun::PassCost cost;
        std::uint32_t mute_poll = 1;

        for (;;) {
            if (stop_flag->load(std::memory_order_relaxed)) {
                break;
            }
            const auto pass_start = std::chrono::steady_clock::now();

            if (const std::int64_t request =
                    seek_request->exchange(0, std::memory_order_acq_rel);
                request != 0) {
                const auto target = static_cast<MIDITick>(request - 1);
                if (tracks_ref->mutex.try_lock_shared()) {
                    {
                        std::lock_guard device_lock(device_ref->mutex);
                        run.seek_to(*device_ref->value, target, tracks_ref->value);
                    }
                    tracks_ref->mutex.unlock_shared();

                    music_start_secs = tempo_copy.ticks_to_secs_from_map(ppq_copy, target);
                    play_start = std::chrono::steady_clock::now();
                    playback_pos->store(target, std::memory_order_relaxed);
                    publish_clock();
                } else {
                    seek_request->store(request, std::memory_order_release);
                }
            }

            const double elapsed =
                std::chrono::duration<double>(std::chrono::steady_clock::now() - play_start)
                    .count();
            const MIDITick now = tempo_copy.secs_to_ticks_from_map(
                ppq_copy, static_cast<float>(music_start_secs + elapsed));
            playback_pos->store(now, std::memory_order_relaxed);

            // try_lock only, tracks before device: an edit must never stall playback
            if (tracks_ref->mutex.try_lock_shared()) {
                const auto& trks = tracks_ref->value;
                if ((mute_poll++ & 7u) == 0) {
                    for (std::size_t t = 0; t < trks.size(); ++t) {
                        run.set_muted(t, trks[t].muted);
                    }
                }

                {
                    std::lock_guard device_lock(device_ref->mutex);
                    run.update_tick(*device_ref->value, now, trks, probe_on ? &cost : nullptr);
                }
                tracks_ref->mutex.unlock_shared();
            }

            if (probe_on) {
                const auto pass_end = std::chrono::steady_clock::now();
                const double us =
                    std::chrono::duration<double, std::micro>(pass_end - pass_start).count();
                worst_pass_us = std::max(worst_pass_us, us);
                total_pass_us += us;
                passes += 1;
                if (now > last_reported_tick) {
                    ticks_advanced += now - last_reported_tick;
                }
                last_reported_tick = now;

                if (std::chrono::duration<double>(pass_end - probe_since).count() >= 1.0) {
                    std::printf("[pass] %zu/s, mean %.0f us, worst %.0f us | offs %.0f, gather %.0f, "
                                "sort %.0f, send %.0f us/s | %zu note-ons, %zu events\n",
                                passes, total_pass_us / static_cast<double>(passes),
                                worst_pass_us, cost.offs, cost.gather, cost.sort, cost.send,
                                cost.note_ons, cost.events);
                    cost = PlaybackRun::PassCost{};
                    std::fflush(stdout);
                    passes = 0;
                    total_pass_us = 0.0;
                    worst_pass_us = 0.0;
                    ticks_advanced = 0;
                    probe_since = pass_end;
                }
            }

            clock.sleep_one_ms();
        }

        {
            std::lock_guard lock(transport_clock->mutex);
            transport_clock->value.running = false;
        }

        std::lock_guard device_lock(device_ref->mutex);
        run.all_notes_off(*device_ref->value);
    }).detach();
}

void PlaybackManager::toggle_playback() {
    if (!playing) {
        start_playback();
        playing = true;
    } else {
        stop();
        playing = false;
    }
}

void PlaybackManager::switch_device(SharedDevice new_device) {
    const MIDITick last_tick = playback_pos_ticks->load(std::memory_order_relaxed);
    if (playing) {
        stop();
        reset_events();
    }

    {
        std::lock_guard lock(device->mutex);
        device->value->close_stream();
    }

    device = std::move(new_device);

    {
        std::lock_guard lock(device->mutex);
        device->value->init_audio();
    }

    if (playing) {
        navigate_to(last_tick);
        start_playback();
    }
}

}
