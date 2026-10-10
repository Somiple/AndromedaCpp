#include "midi/midi_export.h"

#include <algorithm>
#include <atomic>
#include <exception>
#include <new>
#include <string_view>
#include <system_error>
#include <thread>

#include "midi/export/export_encode.h"
#include "midi/export/export_file.h"
#include "midi/export/export_pipeline.h"
#include "midi/export/export_plan.h"

// the export in four parts: export_plan cuts the tracks into units of work, export_encode
// turns a unit into bytes, export_pipeline runs the units on threads and puts their bytes in
// file order, export_file is the file. this file checks the input, starts the pipeline and
// holds the slow path for input that is not in order

namespace andromeda::midi {

namespace {

using namespace exporter;

// sorted copies of what was out of order, one entry per track, empty where none was needed
struct Prepared {
    std::vector<std::vector<Note>> notes;
    std::vector<std::vector<ChannelEvent>> channel;
};

// runs body(index) for every index below count on up to the given number of threads, the
// calling one included. an exception in any of them ends the rest early and comes back as
// bad_alloc, the only kind the bodies here throw
template <typename Body>
void for_each_index(std::size_t count, unsigned threads, Body&& body) {
    std::atomic<std::size_t> next{0};
    std::atomic<bool> failed{false};
    const auto work = [&]() noexcept {
        try {
            for (;;) {
                const std::size_t index = next.fetch_add(1, std::memory_order_relaxed);
                if (index >= count || failed.load(std::memory_order_relaxed)) {
                    return;
                }
                body(index);
            }
        } catch (...) {
            failed.store(true);
        }
    };
    {
        std::vector<std::jthread> pool;
        const std::size_t extra = std::min<std::size_t>(threads, count) - (count > 0 ? 1 : 0);
        try {
            for (std::size_t i = 0; i < extra; ++i) {
                pool.emplace_back(work);
            }
        } catch (const std::exception&) {
            // fewer threads than asked for still do the job
        }
        work();
    }
    if (failed.load()) {
        throw std::bad_alloc();
    }
}

// stable sort by start: counting passes over 11 bit digits, lowest first
void sort_by_start(std::vector<Note>& notes) {
    std::vector<Note> spare(notes.size());
    std::uint32_t used = 0;
    for (const Note& note : notes) {
        used |= note.start;
    }
    for (unsigned shift = 0; shift < 32 && (used >> shift) != 0; shift += 11) {
        std::uint32_t counts[1u << 11] = {};
        for (const Note& note : notes) {
            ++counts[(note.start >> shift) & 0x7FF];
        }
        std::uint32_t total = 0;
        for (std::uint32_t& count : counts) {
            const std::uint32_t n = count;
            count = total;
            total += n;
        }
        for (const Note& note : notes) {
            spare[counts[(note.start >> shift) & 0x7FF]++] = note;
        }
        notes.swap(spare);
    }
}

// the notes of a track are checked in stretches of this many, so that the threads share one
// huge track as well as many small ones
constexpr std::size_t CHECK_NOTES = std::size_t{1} << 20;

// notes [begin, end) of a track and what the check of them found
struct Stretch {
    std::size_t track = 0;
    std::size_t begin = 0;
    std::size_t end = 0;
    // the last tick one of the notes starts at or, where that is a tick, ends at
    std::uint32_t last_tick = 0;
    // every note starts at or behind the one before it, the note before the stretch included
    bool ordered = true;
};

// finds the end tick of every track and gives tracks that are out of order a sorted copy
void prepare(Plan& plan, const std::vector<MIDITrack>& tracks, Prepared& prepared,
             unsigned threads) {
    prepared.notes.resize(tracks.size());
    prepared.channel.resize(tracks.size());

    std::vector<Stretch> stretches;
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const std::size_t count = tracks[t].notes.size();
        for (std::size_t begin = 0; begin < count; begin += CHECK_NOTES) {
            stretches.push_back(Stretch{t, begin, std::min(begin + CHECK_NOTES, count)});
        }
    }
    // every index touches its own stretch only
    for_each_index(stretches.size(), threads, [&](std::size_t s) {
        Stretch& stretch = stretches[s];
        const Note* const notes = tracks[stretch.track].notes.data();
        bool ordered = true;
        std::uint32_t last = 0;
        std::uint32_t prev = stretch.begin > 0 ? notes[stretch.begin - 1].start : 0;
        for (std::size_t i = stretch.begin; i < stretch.end; ++i) {
            const Note& note = notes[i];
            ordered &= note.start >= prev;
            prev = note.start;
            last = std::max(last, note.start);
            const std::uint64_t wide = static_cast<std::uint64_t>(note.start) + note.length;
            if (wide <= LAST_TICK) {
                last = std::max(last, static_cast<std::uint32_t>(wide));
            }
        }
        stretch.last_tick = last;
        stretch.ordered = ordered;
    });
    std::vector<char> ordered(tracks.size(), 1);
    for (const Stretch& stretch : stretches) {
        TrackView& view = plan.tracks[stretch.track];
        view.track_end = std::max(view.track_end, stretch.last_tick);
        if (!stretch.ordered) {
            ordered[stretch.track] = 0;
        }
    }

    // every index touches its own track only
    for_each_index(tracks.size(), threads, [&](std::size_t t) {
        TrackView& view = plan.tracks[t];
        const MIDITrack& track = tracks[t];
        if (track.notes.empty()) {
            return;
        }
        if (ordered[t] == 0) {
            prepared.notes[t] = track.notes;
            sort_by_start(prepared.notes[t]);
            view.notes = prepared.notes[t].data();
        }
        const auto before = [](const ChannelEvent& a, const ChannelEvent& b) {
            return a.tick < b.tick;
        };
        if (!std::is_sorted(track.channel_events.begin(), track.channel_events.end(), before)) {
            prepared.channel[t] = track.channel_events;
            std::stable_sort(prepared.channel[t].begin(), prepared.channel[t].end(), before);
            view.channel = prepared.channel[t].data();
        }
    });
    plan.prepared = true;
}

// one go at the file with the plan as it stands
[[nodiscard]] PipelineResult write_plan(const Plan& plan, OutputFile& file, unsigned threads) {
    if (!file.reserve(plan.size_floor)) {
        return PipelineResult{Outcome::file_failed, "could not write the file", 0};
    }
    // a thread without a job of its own has nothing to do
    const unsigned workers =
        static_cast<unsigned>(std::min<std::size_t>(threads, plan.jobs.size()));
    const PipelineResult result = run_pipeline(plan, file, workers);
    if (result.outcome == Outcome::done && !file.finish(result.size)) {
        return PipelineResult{Outcome::file_failed, "could not write the file", 0};
    }
    return result;
}

// what the user gets to read when the file fails: what was tried, the path, and the
// system's reason where it gave one
[[nodiscard]] std::string file_error(std::string_view what, const std::filesystem::path& path,
                                     const OutputFile& file) {
    std::string message(what);
    try {
        const std::u8string name = path.u8string();
        message.append(reinterpret_cast<const char*>(name.data()), name.size());
    } catch (const std::system_error&) {
        // a name that is not valid unicode has no utf-8 form
        message += "the file";
    }
    const std::string reason = error_text(file.error());
    if (!reason.empty()) {
        message += ": ";
        message += reason;
    }
    return message;
}

std::expected<void, std::string> export_checked(const std::filesystem::path& path,
                                                std::uint16_t ppq,
                                                const std::vector<MetaEvent>& global_metas,
                                                const std::vector<MIDITrack>& tracks,
                                                const MIDIExportOptions& options) {
    if (!processor_has_avx2()) {
        return std::unexpected("the export needs a processor with avx2");
    }
    if (tracks.size() + 1 > 0xFFFF) {
        return std::unexpected("too many tracks");
    }

    Plan plan;
    plan.ppq = ppq;
    plan.metas = &global_metas;
    const auto meta_before = [](const MetaEvent& a, const MetaEvent& b) { return a.tick < b.tick; };
    if (!std::is_sorted(global_metas.begin(), global_metas.end(), meta_before)) {
        plan.meta_order.reserve(global_metas.size());
        for (const MetaEvent& meta : global_metas) {
            plan.meta_order.push_back(&meta);
        }
        std::stable_sort(plan.meta_order.begin(), plan.meta_order.end(),
                         [](const MetaEvent* a, const MetaEvent* b) { return a->tick < b->tick; });
    }
    plan.conductor_bytes = conductor_size(plan);
    if (plan.conductor_bytes > 0xFFFFFFFFull) {
        return std::unexpected("track length overflow");
    }

    plan.tracks.resize(tracks.size());
    for (std::size_t t = 0; t < tracks.size(); ++t) {
        const MIDITrack& track = tracks[t];
        // a track without notes exports as an empty track: its view stays empty
        if (track.notes.empty()) {
            continue;
        }
        // that many events do not fit a track either way, and the counts are kept in 32 bits
        if (track.notes.size() > LAST_TICK || track.channel_events.size() > LAST_TICK) {
            return std::unexpected("track length overflow");
        }
        // a note takes 8 bytes at least and a channel event 3: a track that cannot fit is
        // turned down before anything is written
        if (8ull * track.notes.size() + 3ull * track.channel_events.size() + 4 > 0xFFFFFFFFull) {
            return std::unexpected("track length overflow");
        }
        TrackView& view = plan.tracks[t];
        view.notes = track.notes.data();
        view.note_count = static_cast<std::uint32_t>(track.notes.size());
        view.channel = track.channel_events.data();
        view.channel_count = static_cast<std::uint32_t>(track.channel_events.size());
    }

    const unsigned threads =
        options.threads != 0 ? options.threads : std::max(1u, std::thread::hardware_concurrency());

    // the file only stays when all of it is written: it goes with this object otherwise
    OutputFile file;
    if (!file.create(path)) {
        return std::unexpected(file_error("could not create ", path, file));
    }

    // input is expected in order. only when a worker finds otherwise is everything checked,
    // sorted where needed, and the file started again
    plan_units(plan, options.grain);
    PipelineResult result = write_plan(plan, file, threads);
    Prepared prepared;
    if (result.outcome == Outcome::needs_prepare) {
        prepare(plan, tracks, prepared, threads);
        plan_units(plan, options.grain);
        file.restart();
        result = write_plan(plan, file, threads);
    }
    if (result.outcome == Outcome::file_failed) {
        return std::unexpected(file_error("could not write ", path, file));
    }
    if (result.outcome != Outcome::done) {
        return std::unexpected(result.message);
    }
    return {};
}

}

std::expected<void, std::string> export_midi_file(const std::filesystem::path& path,
                                                  std::uint16_t ppq,
                                                  const std::vector<MetaEvent>& global_metas,
                                                  const std::vector<MIDITrack>& tracks,
                                                  const MIDIExportOptions& options) {
    // nothing may leave this function. the two fixed messages are short enough to need no memory
    try {
        return export_checked(path, ppq, global_metas, tracks, options);
    } catch (const std::bad_alloc&) {
        return std::unexpected("out of memory");
    } catch (const std::exception& e) {
        try {
            return std::unexpected(std::string(e.what()));
        } catch (...) {
        }
    } catch (...) {
    }
    return std::unexpected("export failed");
}

}
