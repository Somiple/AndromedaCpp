#include "midi/export/export_plan.h"

#include <bit>
#include <compare>

namespace andromeda::midi::exporter {

namespace {

// events to a unit of work. smaller units spread the work more evenly and keep a unit's
// notes, note offs and output in the cache of a core; larger ones cost less bookkeeping
constexpr std::size_t DEFAULT_GRAIN = 32768;

// a grain too small for the input is raised until the input is this many grains: every unit
// costs some hundred bytes of plan, so more of them only take memory. small inputs are
// still cut as finely as asked
constexpr std::uint64_t MAX_UNITS_BY_GRAIN = 1u << 17;

// the output buffers are sized for the heaviest planned job at 4.5 bytes an event, four for
// an event with a delta of one byte and the rest for longer deltas, and never below the spare
constexpr std::size_t BUFFER_BYTES_PER_EVENT_X2 = 9;
constexpr std::size_t BUFFER_BYTES_SPARE = 4096;

// where the note offs of a track lie is only known by reading every note, which planning
// has no time for. so one note of every unit's worth of notes is read as a sample, and a
// unit whose share of the note offs holds this many samples is taken to hold too many: it
// is cut behind every second one of them. fewer would cut units that are only a little
// heavy, more would leave heavier ones whole
constexpr std::size_t PILE_SAMPLES = 4;

// slots of the route table for each unit. more of them lie in a single unit, which spares
// the lookup its search; each costs 8 bytes
constexpr std::uint64_t ROUTE_SLOTS_PER_UNIT = 16;

// plain loops instead of the standard searches: these also run on unsorted input, where any
// in range answer will do because the scan rejects the track afterwards

// notes with a start below the tick
[[nodiscard]] std::uint32_t notes_before(const TrackView& view, std::uint64_t tick) {
    std::uint32_t low = 0;
    std::uint32_t high = view.note_count;
    while (low < high) {
        const std::uint32_t mid = low + (high - low) / 2;
        if (view.notes[mid].start < tick) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

// channel events with a tick below the given one
[[nodiscard]] std::uint32_t channel_before(const TrackView& view, std::uint64_t tick) {
    std::uint32_t low = 0;
    std::uint32_t high = view.channel_count;
    if (high > 0 && view.channel[high - 1].tick < tick) {
        return high;
    }
    while (low < high) {
        const std::uint32_t mid = low + (high - low) / 2;
        if (view.channel[mid].tick < tick) {
            low = mid + 1;
        } else {
            high = mid;
        }
    }
    return low;
}

// a position between two events of a track: the note ons below on, the note offs up to the
// key off and the channel events below ch come before it. a tick of a track holds the note
// offs of earlier notes first, in note order, then its note ons, each with its own note off
// when the note has no length, then its channel events. so each of the three kinds of cut
// below has a prefix of the track before it, and cuts in sorted order have growing prefixes
struct Cut {
    std::uint64_t off = 0;
    std::uint32_t on = 0;
    std::uint32_t ch = 0;

    auto operator<=>(const Cut&) const = default;
};

// kept between tracks so that a project of many tracks does not allocate for each
struct Workspace {
    std::vector<Cut> cuts;
    std::vector<std::uint64_t> samples;
};

// a place in a stretch of notes that no regular pattern of a track keeps step with
[[nodiscard]] std::size_t scattered(std::size_t stretch, std::size_t length) {
    return static_cast<std::size_t>((stretch * 0x9E3779B97F4A7C15ull >> 32) % length);
}

// the keys of the note offs of one note in every stretch of note_step notes, sorted. notes
// that end where they start have no note off of their own place, and an end past the last
// tick is not known before the track was prepared
void sample_note_offs(const TrackView& view, std::size_t note_step, bool prepared,
                      std::vector<std::uint64_t>& samples) {
    samples.clear();
    const std::size_t notes = view.note_count;
    for (std::size_t begin = 0; begin < notes; begin += note_step) {
        const std::size_t at =
            begin + scattered(begin / note_step, std::min(note_step, notes - begin));
        const Note& note = view.notes[at];
        const std::uint64_t wide = static_cast<std::uint64_t>(note.start) + note.length;
        if (wide > LAST_TICK && !prepared) {
            continue;
        }
        const std::uint32_t end =
            wide > LAST_TICK ? view.track_end : static_cast<std::uint32_t>(wide);
        if (end > note.start) {
            samples.push_back(off_key(end, static_cast<std::uint32_t>(at)));
        }
    }
    std::sort(samples.begin(), samples.end());
}

// units [first_unit, units.size()) are the units of the track, in order
void build_route(Route& route, const std::vector<Unit>& units, std::uint32_t first_unit,
                 std::uint32_t first_on, std::uint32_t last_on) {
    const std::uint32_t last_unit = static_cast<std::uint32_t>(units.size() - 1);
    // the units that end up to the last note on, the units that end behind it, and the last
    // tick one of those ends at. the last unit of the track has no end
    std::uint64_t body_units = 0;
    std::uint64_t tail_units = 0;
    const std::uint64_t tail_origin = static_cast<std::uint64_t>(last_on) + 1;
    std::uint64_t tail_top = tail_origin;
    for (std::uint32_t u = first_unit; u < last_unit; ++u) {
        const std::uint32_t tick = static_cast<std::uint32_t>(units[u].off_until >> 32);
        if (tick <= last_on) {
            ++body_units;
        } else {
            ++tail_units;
            tail_top = tick;
        }
    }

    // the first unit, from the one given on, whose note offs reach the key
    std::uint32_t unit = first_unit;
    const auto holder = [&](std::uint64_t key) {
        while (unit < last_unit && units[unit].off_until < key) {
            ++unit;
        }
        return unit;
    };
    // even slots for the ticks from origin up to top, about so many for each of the units
    // that end there, and one slot for every tick behind them
    const auto add_scale = [&](Route::Scale& scale, std::uint64_t origin, std::uint64_t top,
                               std::uint64_t unit_count) {
        const std::uint64_t span = top > origin ? top - origin : 0;
        scale.origin = static_cast<std::uint32_t>(origin);
        scale.shift = static_cast<std::uint32_t>(
            std::bit_width(span / (ROUTE_SLOTS_PER_UNIT * std::max<std::uint64_t>(unit_count, 1))));
        const std::uint64_t even = (span >> scale.shift) + 1;
        scale.first_slot = static_cast<std::uint32_t>(route.slots.size());
        scale.slot_count = static_cast<std::uint32_t>(even + 1);
        unit = first_unit;
        for (std::uint64_t slot = 0; slot <= even; ++slot) {
            const std::uint64_t begin = origin + (slot << scale.shift);
            const std::uint64_t end =
                slot < even ? origin + ((slot + 1) << scale.shift) - 1 : LAST_TICK;
            const std::uint32_t first = holder(
                off_key(static_cast<std::uint32_t>(std::min<std::uint64_t>(begin, LAST_TICK)), 0));
            const std::uint32_t last = holder(off_key(
                static_cast<std::uint32_t>(std::min<std::uint64_t>(end, LAST_TICK)), EVERY_NOTE));
            route.slots.push_back(Route::Slot{first, last});
            unit = first;
        }
    };
    route.last_on = last_on;
    add_scale(route.scales[0], first_on, last_on, body_units);
    add_scale(route.scales[1], tail_origin, tail_top, tail_units);
}

void plan_track(std::uint32_t index, const TrackView& view, std::size_t grain, bool prepared,
                std::vector<Unit>& units, Route& route, Workspace& workspace) {
    const std::uint32_t first_unit = static_cast<std::uint32_t>(units.size());
    const std::uint32_t notes = view.note_count;
    const std::uint32_t channel = view.channel_count;
    // a note is two events
    const std::size_t note_step = std::max<std::size_t>(grain / 2, 1);

    Unit unit;
    unit.track = index;
    if (notes <= note_step && channel <= grain) {
        unit.on_end = notes;
        unit.ch_end = channel;
        unit.off_until = off_key(LAST_TICK, EVERY_NOTE);
        unit.whole_track = true;
        unit.last = true;
        units.push_back(unit);
        return;
    }

    std::vector<Cut>& cuts = workspace.cuts;
    cuts.clear();
    // before a note on: the note offs of its tick are before the cut, the channel events of
    // its tick behind it
    for (std::size_t on = note_step; on < notes; on += note_step) {
        const std::uint32_t tick = view.notes[on].start;
        cuts.push_back(Cut{off_key(tick, EVERY_NOTE), static_cast<std::uint32_t>(on),
                           channel_before(view, tick)});
    }
    // before a channel event: every note event of its tick is before the cut
    for (std::size_t ch = grain; ch < channel; ch += grain) {
        const std::uint32_t tick = view.channel[ch].tick;
        cuts.push_back(Cut{off_key(tick, EVERY_NOTE),
                           notes_before(view, static_cast<std::uint64_t>(tick) + 1),
                           static_cast<std::uint32_t>(ch)});
    }
    std::sort(cuts.begin(), cuts.end());

    // behind a note off, where the cuts so far leave too many note offs in one unit: after a
    // stretch without note ons, on a tick that many notes end on, behind the last note on.
    // the note ons and channel events of the tick are behind the cut. such a cut can lie
    // inside the note offs of one tick, which is the only way to divide a tick that holds
    // millions of them
    const std::vector<std::uint64_t>& samples = workspace.samples;
    sample_note_offs(view, note_step, prepared, workspace.samples);
    const std::size_t plain_cuts = cuts.size();
    std::size_t sample = 0;
    for (std::size_t c = 0; c <= plain_cuts; ++c) {
        const std::uint64_t until = c < plain_cuts ? cuts[c].off : off_key(LAST_TICK, EVERY_NOTE);
        const std::size_t first = sample;
        while (sample < samples.size() && samples[sample] <= until) {
            ++sample;
        }
        if (sample - first < PILE_SAMPLES) {
            continue;
        }
        for (std::size_t s = first + 1; s + 1 < sample; s += 2) {
            const std::uint32_t tick = static_cast<std::uint32_t>(samples[s] >> 32);
            const bool same_tick = cuts.size() > plain_cuts && cuts.back().off >> 32 == tick;
            cuts.push_back(Cut{samples[s], same_tick ? cuts.back().on : notes_before(view, tick),
                               same_tick ? cuts.back().ch : channel_before(view, tick)});
        }
    }
    // the new cuts are in order among themselves
    std::inplace_merge(cuts.begin(), cuts.begin() + static_cast<std::ptrdiff_t>(plain_cuts),
                       cuts.end());

    const auto add = [&](const Cut& from, const Cut& to, bool last) {
        unit.on_begin = from.on;
        unit.on_end = to.on;
        unit.ch_begin = from.ch;
        unit.ch_end = to.ch;
        unit.off_until = to.off;
        unit.last = last;
        units.push_back(unit);
    };
    Cut prev;
    for (Cut cut : cuts) {
        // only unsorted input gives cuts that step back; keep the ranges well formed for the scan
        cut.on = std::max(cut.on, prev.on);
        cut.ch = std::max(cut.ch, prev.ch);
        // a cut at the place of the one before it would make a unit without events
        if (cut != prev) {
            add(prev, cut, false);
            prev = cut;
        }
    }
    // always there, even when empty: it writes the end of track
    add(prev, Cut{off_key(LAST_TICK, EVERY_NOTE), notes, channel}, true);
    build_route(route, units, first_unit, view.notes[0].start, view.notes[notes - 1].start);
}

void plan_jobs(Plan& plan) {
    Job prelude;
    prelude.kind = JobKind::prelude;
    prelude.bytes = static_cast<std::size_t>(14 + 8 + plan.conductor_bytes);
    plan.jobs.push_back(prelude);

    // a whole track weighs its events and three more for its chunk header and end of track
    std::size_t weight = 0;
    std::size_t heaviest = 0;
    for (std::uint32_t u = 0; u < plan.units.size(); ++u) {
        const Unit& unit = plan.units[u];
        const std::size_t events = 2 * static_cast<std::size_t>(unit.on_end - unit.on_begin) +
                                   (unit.ch_end - unit.ch_begin);
        if (!unit.whole_track) {
            Job job;
            job.kind = JobKind::segment;
            job.first_unit = u;
            job.unit_count = 1;
            plan.jobs.push_back(job);
            weight = 0;
            heaviest = std::max(heaviest, events);
            continue;
        }
        if (weight == 0 || weight + events + 3 > plan.grain) {
            Job job;
            job.kind = JobKind::batch;
            job.first_unit = u;
            plan.jobs.push_back(job);
            weight = 0;
        }
        Job& job = plan.jobs.back();
        ++job.unit_count;
        job.bytes += 12 + 4 * events;
        weight += events + 3;
        heaviest = std::max(heaviest, weight);
    }
    // sized by the jobs that are there, so that a grain beyond the whole input does not make
    // the buffers huge, and by no more than a grain: only a unit full of both notes and
    // channel events is planned heavier, and it gets a buffer of its own size
    plan.buffer_bytes =
        BUFFER_BYTES_PER_EVENT_X2 * std::min(plan.grain, heaviest) / 2 + BUFFER_BYTES_SPARE;
}

}

void plan_units(Plan& plan, std::size_t grain) {
    std::uint64_t events = 0;
    // a chunk header and an end of track for every track, eight bytes for the two events of
    // a note and three for a channel event: nothing can be shorter
    plan.size_floor =
        14 + 8 + plan.conductor_bytes + 12 * static_cast<std::uint64_t>(plan.tracks.size());
    for (const TrackView& view : plan.tracks) {
        events += 2 * static_cast<std::uint64_t>(view.note_count) + view.channel_count;
        plan.size_floor += 8 * static_cast<std::uint64_t>(view.note_count) +
                           3 * static_cast<std::uint64_t>(view.channel_count);
    }
    plan.grain = static_cast<std::size_t>(
        std::max<std::uint64_t>(grain != 0 ? grain : DEFAULT_GRAIN, events / MAX_UNITS_BY_GRAIN));
    plan.track_units.clear();
    plan.units.clear();
    plan.jobs.clear();
    plan.routes.assign(plan.tracks.size(), Route{});
    Workspace workspace;
    for (std::uint32_t t = 0; t < plan.tracks.size(); ++t) {
        plan.track_units.push_back(static_cast<std::uint32_t>(plan.units.size()));
        plan_track(t, plan.tracks[t], plan.grain, plan.prepared, plan.units, plan.routes[t],
                   workspace);
    }
    plan.track_units.push_back(static_cast<std::uint32_t>(plan.units.size()));
    plan_jobs(plan);
}

}
