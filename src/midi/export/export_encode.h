#pragma once

// the bytes of the exporter: what a unit of a track, and the start of the file, look like
// in the file. nothing here knows about threads. private to the exporter

#include <cstddef>
#include <cstdint>
#include <cstring>

#include "midi/export/export_plan.h"

namespace andromeda::midi::exporter {

// delta 0, meta event 2F, no data
inline constexpr std::uint32_t END_OF_TRACK = 0x002FFF00u;

// the merge writes whole words and blocks of eight words: its last store can reach this far
// past the last byte it keeps, so every buffer it writes to has that much room at the end
inline constexpr std::size_t MERGE_SLACK = 32;

inline void store32(std::uint8_t* out, std::uint32_t value) { std::memcpy(out, &value, 4); }

inline void store_be32(std::uint8_t* out, std::uint32_t value) {
    out[0] = static_cast<std::uint8_t>(value >> 24);
    out[1] = static_cast<std::uint8_t>(value >> 16);
    out[2] = static_cast<std::uint8_t>(value >> 8);
    out[3] = static_cast<std::uint8_t>(value);
}

// a full 32 bit value takes five bytes, like the writer before this one wrote it
[[nodiscard]] inline std::size_t vlq_length(std::uint32_t value) {
    return value < 0x80         ? 1
           : value < 0x4000     ? 2
           : value < 0x200000   ? 3
           : value < 0x10000000 ? 4
                                : 5;
}

inline std::uint8_t* put_vlq(std::uint8_t* out, std::uint32_t value) {
    const std::size_t length = vlq_length(value);
    for (std::size_t i = 0; i < length; ++i) {
        const std::uint32_t group = (value >> (7 * (length - 1 - i))) & 0x7F;
        out[i] = static_cast<std::uint8_t>(i + 1 < length ? group | 0x80 : group);
    }
    return out + length;
}

// a note off waiting for its place is a record: its tick in the high half, and in the low
// half the event as it goes into the file behind a delta of zero
[[nodiscard]] inline std::uint32_t record_tick(std::uint64_t record) {
    return static_cast<std::uint32_t>(record >> 32);
}

// the channel goes into the status as it is, like the writer before this one did it
[[nodiscard]] inline std::uint64_t off_record(std::uint32_t tick, const Note& note) {
    const std::uint32_t status = static_cast<std::uint8_t>(0x80 | note.channel);
    return static_cast<std::uint64_t>(tick) << 32 | status << 8 |
           static_cast<std::uint32_t>(note.key) << 16;
}

// the record of the note off with the given key
[[nodiscard]] inline std::uint64_t off_record(const TrackView& view, std::uint64_t key) {
    return off_record(static_cast<std::uint32_t>(key >> 32),
                      view.notes[static_cast<std::uint32_t>(key)]);
}

struct ScanResult {
    std::size_t local = 0;
    std::size_t ahead = 0;
    // every note starts at or behind the one before it, the last note before the unit included
    bool ordered = true;
    // some note ends past the last tick: its real end is the end of the track
    bool overflow = false;
};

// goes through the notes of a unit: the note offs the unit emits itself go to local, as
// records, and the ones a later unit emits to ahead, as keys: where they go depends on the
// key. both in note order and both with room for an entry per note. note offs of notes that
// end where they start are left out: the merge writes them
[[nodiscard]] ScanResult scan_notes(const TrackView& view, const Unit& unit, std::uint64_t* local,
                                    std::uint64_t* ahead);

// records in tick order need no sort, and so no array to sort them into
[[nodiscard]] bool in_tick_order(const std::uint64_t* records, std::size_t count);

// stable sort by tick, whatever the ticks are. spare has room for count records. returns the
// array that holds the result, one of the two
[[nodiscard]] std::uint64_t* sort_by_tick(std::uint64_t* records, std::uint64_t* spare,
                                          std::size_t count);

// where the output stands: next byte, tick of the last event, next channel event
struct Cursor {
    std::uint8_t* out = nullptr;
    std::uint32_t tick = 0;
    const ChannelEvent* channel = nullptr;
};

// writes the events of a unit: its note ons, the given note offs, sorted by tick and equal
// ticks in note order, and its channel events from the cursor's on. the three have to be in
// tick order each. the output needs the room that measure gives and the slack behind it
[[nodiscard]] Cursor merge_events(Cursor start, const TrackView& view, const Unit& unit,
                                  const std::uint64_t* offs, std::size_t off_count);

[[nodiscard]] bool channel_in_order(const TrackView& view, const Unit& unit);

// the first and the last tick of a unit's events and the most bytes they take, the first
// delta counted as one byte
struct Extent {
    std::uint32_t first = LAST_TICK;
    std::uint32_t last = 0;
    std::size_t bytes = 0;
};

[[nodiscard]] Extent measure(const TrackView& view, const Unit& unit, const std::uint64_t* offs,
                             std::size_t off_count);

// the body of the conductor track
[[nodiscard]] std::uint64_t conductor_size(const Plan& plan);

// writes the file header and the whole conductor track, 22 bytes and the conductor size
std::uint8_t* put_prelude(const Plan& plan, std::uint8_t* out);

}
