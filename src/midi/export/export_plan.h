#pragma once

// how the exporter cuts a project into pieces of work. private to the exporter

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "midi/events/channel_event.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"

namespace andromeda::midi::exporter {

inline constexpr std::uint32_t LAST_TICK = 0xFFFFFFFFu;
// in the place of a note position: every note off of the tick. no note has this position, a
// track holds fewer notes
inline constexpr std::uint32_t EVERY_NOTE = 0xFFFFFFFFu;

struct TrackView {
    const Note* notes = nullptr;
    const ChannelEvent* channel = nullptr;
    std::uint32_t note_count = 0;
    // zero for a track without notes: its channel events are not exported
    std::uint32_t channel_count = 0;
    // where notes with an overflowing end stop; only known once the track was prepared
    std::uint32_t track_end = 0;
};

// the place of a note off among the note offs of its track: they go by tick, and those of one
// tick by the position of their note. a tick with EVERY_NOTE stands behind all of the tick
[[nodiscard]] inline std::uint64_t off_key(std::uint32_t tick, std::uint32_t note) {
    return static_cast<std::uint64_t>(tick) << 32 | note;
}

// one piece of a track in file order: the note ons [on_begin, on_end) with the note offs of
// notes that end where they start, the channel events [ch_begin, ch_end), and the other note
// offs with a key behind the off_until of the unit before and up to its own
struct Unit {
    std::uint64_t off_until = 0;
    std::uint32_t track = 0;
    std::uint32_t on_begin = 0;
    std::uint32_t on_end = 0;
    std::uint32_t ch_begin = 0;
    std::uint32_t ch_end = 0;
    // the only unit of its track; small tracks are not cut up
    bool whole_track = false;
    // the last unit of its track: it also writes the end of track
    bool last = false;
};

// finds the unit of a cut up track that a note off belongs to. the ticks up to the last note
// on and the ticks behind it are each divided into even slots, and a table gives the first
// and the last unit that note offs of a slot can belong to. nearly every slot lies in one
// unit or two, so that the lookup rarely has to search
struct Route {
    struct Slot {
        std::uint32_t first = 0;
        std::uint32_t last = 0;
    };

    // slot k holds the ticks from origin + (k << shift) on; the last slot holds every tick
    // behind the even ones
    struct Scale {
        std::uint32_t origin = 0;
        std::uint32_t shift = 0;
        std::uint32_t slot_count = 1;
        std::uint32_t first_slot = 0;
    };

    std::uint32_t last_on = 0;
    // for the ticks up to the last note on, and for the ticks behind it
    Scale scales[2];
    std::vector<Slot> slots;

    // units are the units of the whole plan; the answer is always a unit of the track. hint
    // is the answer for the note off before this one
    [[nodiscard]] std::uint32_t unit_for(const Unit* units, std::uint64_t key,
                                         std::uint32_t hint) const {
        const std::uint32_t tick = static_cast<std::uint32_t>(key >> 32);
        const Scale& scale = scales[tick > last_on ? 1 : 0];
        // unsorted input may ask for ticks before the origin; any slot will do then
        const std::uint64_t index = std::min<std::uint64_t>(
            static_cast<std::uint64_t>(tick - scale.origin) >> scale.shift, scale.slot_count - 1);
        const Slot slot = slots[scale.first_slot + static_cast<std::size_t>(index)];
        if (slot.last - slot.first > 1) [[unlikely]] {
            return search(units, key, slot, hint);
        }
        // one unit or two: decided without a branch, there is no telling which it is
        return slot.first + static_cast<std::uint32_t>((slot.first != slot.last) &
                                                       (units[slot.first].off_until < key));
    }

    // for a slot of many units. where a tick that many note offs lie on was cut into them,
    // the note offs of a scan, which come in note order, stay in one of those units for long:
    // the unit of the note off before is looked at first
    [[nodiscard]] std::uint32_t search(const Unit* units, std::uint64_t key, Slot slot,
                                       std::uint32_t hint) const {
        std::uint32_t low = slot.first;
        std::uint32_t high = slot.last;
        if (low < hint && hint <= high && units[hint - 1].off_until < key) {
            low = hint;
        }
        if (low == high || units[low].off_until >= key) {
            return low;
        }
        ++low;
        while (low < high) {
            const std::uint32_t mid = low + (high - low) / 2;
            if (units[mid].off_until < key) {
                low = mid + 1;
            } else {
                high = mid;
            }
        }
        return low;
    }
};

enum class JobKind : std::uint8_t { prelude, batch, segment };

// what one worker encodes into one buffer: the file header with the conductor track, a run of
// small whole tracks, or one unit of a track that was cut up
struct Job {
    JobKind kind = JobKind::batch;
    std::uint32_t first_unit = 0;
    std::uint32_t unit_count = 0;
    // bytes the job takes when no delta needs more than one byte
    std::size_t bytes = 0;
};

struct Plan {
    std::uint16_t ppq = 0;
    const std::vector<MetaEvent>* metas = nullptr;
    // empty when the metas are already in tick order
    std::vector<const MetaEvent*> meta_order;
    std::uint64_t conductor_bytes = 0;
    std::vector<TrackView> tracks;
    // units of track t are [track_units[t], track_units[t + 1])
    std::vector<std::uint32_t> track_units;
    // in file order
    std::vector<Unit> units;
    // in file order; together they hold every unit once
    std::vector<Job> jobs;
    // one per track, empty for tracks that are not cut up
    std::vector<Route> routes;
    // the file is at least this long: every event at its shortest
    std::uint64_t size_floor = 0;
    // events to a unit, about
    std::size_t grain = 0;
    // the usual size of a job's output buffer
    std::size_t buffer_bytes = 0;
    // the tracks are known to be in order, with their ends found
    bool prepared = false;

    [[nodiscard]] const MetaEvent& meta(std::size_t i) const {
        return meta_order.empty() ? (*metas)[i] : *meta_order[i];
    }
};

// cuts the tracks as they stand into units and jobs. a grain of zero is the tuned default.
// for tracks in order the events of a unit are consecutive in the track, and the units of a
// track follow each other; for other input the units are merely safe to scan, which finds
// the disorder
void plan_units(Plan& plan, std::size_t grain);

}
