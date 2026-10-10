#include "midi/export/export_encode.h"

#include <algorithm>
#include <bit>
#include <variant>

#include <immintrin.h>

// the scan and the merge are written with avx2 intrinsics and nothing else; the exporter
// checks for the instructions before it runs them
#if !defined(_M_X64) && !defined(__x86_64__)
#error the exporter needs an x64 target
#endif

namespace andromeda::midi::exporter {

namespace {

// the scan leaves the vector path for this many notes when it meets one the path cannot
// take: such notes tend to come in numbers
constexpr std::uint32_t SCALAR_STRETCH = 32;
// up to this many records an insertion sort beats the counting passes
constexpr std::size_t SMALL_SORT = 32;
// the most bits of a digit of the counting sort: wider digits mean fewer passes over the
// records, and counters that no longer fit the first level cache
constexpr unsigned DIGIT_BITS = 11;
// the smallest delta that takes more than one byte
constexpr std::uint32_t WIDE_DELTA = 0x80;

[[nodiscard]] inline std::uint32_t note_end(const Note& note, std::uint32_t track_end) {
    const std::uint64_t end = static_cast<std::uint64_t>(note.start) + note.length;
    return end > LAST_TICK ? track_end : static_cast<std::uint32_t>(end);
}

// the three bytes of a note on above an empty low byte
[[nodiscard]] inline std::uint32_t on_word(const Note& note) {
    const std::uint32_t status = static_cast<std::uint8_t>(0x90 | note.channel);
    return status << 8 | static_cast<std::uint32_t>(note.key) << 16 |
           static_cast<std::uint32_t>(note.velocity) << 24;
}

// the bytes of a channel event, first byte lowest; returns their count
inline std::size_t channel_bytes(const ChannelEvent& event, std::uint32_t& bytes) {
    const std::uint32_t channel = event.channel;
    const auto pack = [&](std::uint32_t status, std::uint32_t first, std::uint32_t second) {
        bytes = static_cast<std::uint8_t>(status | channel) | first << 8 | second << 16;
    };
    if (const auto* e = std::get_if<NoteOff>(&event.event_type)) {
        pack(0x80, e->key, 0);
        return 3;
    }
    if (const auto* e = std::get_if<NoteOn>(&event.event_type)) {
        pack(0x90, e->key, e->velocity);
        return 3;
    }
    if (const auto* e = std::get_if<NoteAftertouch>(&event.event_type)) {
        pack(0xA0, e->key, e->pressure);
        return 3;
    }
    if (const auto* e = std::get_if<Controller>(&event.event_type)) {
        pack(0xB0, e->controller, e->value);
        return 3;
    }
    if (const auto* e = std::get_if<ProgramChange>(&event.event_type)) {
        pack(0xC0, e->program, 0);
        return 2;
    }
    if (const auto* e = std::get_if<ChannelAftertouch>(&event.event_type)) {
        pack(0xD0, e->amount, 0);
        return 2;
    }
    // the last kind there is. an event without a value cannot be made by ordinary means
    const auto* e = std::get_if<PitchBend>(&event.event_type);
    pack(0xE0, e != nullptr ? e->lsb : 0, e != nullptr ? e->msb : 0);
    return 3;
}

// writes the channel events before the given tick. kept out of line so that the merge loops
// stay small
__declspec(noinline) Cursor put_channel_events(Cursor at, const ChannelEvent* end,
                                               std::uint64_t before) {
    while (at.channel != end && at.channel->tick < before) {
        std::uint32_t bytes = 0;
        const std::size_t length = channel_bytes(*at.channel, bytes);
        at.out = put_vlq(at.out, at.channel->tick - at.tick);
        at.tick = at.channel->tick;
        store32(at.out, bytes);
        at.out += length;
        ++at.channel;
    }
    return at;
}

// the scan and the merge handle eight plain notes or note offs at a time and leave everything
// else to the scalar code next to them. the vector code reads notes as raw dwords
static_assert(sizeof(Note) == 12 && offsetof(Note, start) == 0 && offsetof(Note, length) == 4 &&
              offsetof(Note, key) == 8 && offsetof(Note, velocity) == 9 &&
              offsetof(Note, channel) == 10);

struct Notes8 {
    __m256i starts;
    __m256i lengths;
    // key, velocity, channel and a padding byte
    __m256i bytes;
};

// lanes of b and c, as the masks say, over a
template <int B_LANES, int C_LANES>
[[nodiscard]] inline __m256i blend3(__m256i a, __m256i b, __m256i c) {
    return _mm256_blend_epi32(_mm256_blend_epi32(a, b, B_LANES), c, C_LANES);
}

// eight notes are 24 dwords: starts at 0, 3, 6 ..., lengths at 1, 4, 7 ..., the bytes at
// 2, 5, 8 ...
[[nodiscard]] inline Notes8 load_notes8(const Note* notes) {
    const auto* const raw = reinterpret_cast<const __m256i*>(notes);
    const __m256i a = _mm256_loadu_si256(raw);
    const __m256i b = _mm256_loadu_si256(raw + 1);
    const __m256i c = _mm256_loadu_si256(raw + 2);
    const auto lanes = [](__m256i from, int l0, int l1, int l2, int l3, int l4, int l5, int l6,
                          int l7) {
        return _mm256_permutevar8x32_epi32(from, _mm256_setr_epi32(l0, l1, l2, l3, l4, l5, l6, l7));
    };
    Notes8 out;
    out.starts =
        blend3<0x38, 0xC0>(lanes(a, 0, 3, 6, 0, 0, 0, 0, 0), lanes(b, 0, 0, 0, 1, 4, 7, 0, 0),
                           lanes(c, 0, 0, 0, 0, 0, 0, 2, 5));
    out.lengths =
        blend3<0x18, 0xE0>(lanes(a, 1, 4, 7, 0, 0, 0, 0, 0), lanes(b, 0, 0, 0, 2, 5, 0, 0, 0),
                           lanes(c, 0, 0, 0, 0, 0, 0, 3, 6));
    out.bytes =
        blend3<0x1C, 0xE0>(lanes(a, 2, 5, 0, 0, 0, 0, 0, 0), lanes(b, 0, 0, 0, 3, 6, 0, 0, 0),
                           lanes(c, 0, 0, 0, 0, 0, 1, 4, 7));
    return out;
}

// lanes where a <= b, unsigned
[[nodiscard]] inline __m256i not_above(__m256i a, __m256i b) {
    return _mm256_cmpeq_epi32(_mm256_max_epu32(a, b), b);
}

[[nodiscard]] inline unsigned lane_bits(__m256i lanes) {
    return static_cast<unsigned>(_mm256_movemask_ps(_mm256_castsi256_ps(lanes)));
}

// each lane's left neighbour, with first in lane 0
[[nodiscard]] inline __m256i shifted_in(__m256i lanes, std::uint32_t first) {
    return _mm256_blend_epi32(
        _mm256_permutevar8x32_epi32(lanes, _mm256_setr_epi32(0, 0, 1, 2, 3, 4, 5, 6)),
        _mm256_set1_epi32(static_cast<int>(first)), 1);
}

// the event bytes of eight notes above an empty low byte, like on_word and the low half of
// off_record
[[nodiscard]] inline __m256i event_words(__m256i bytes, int status) {
    const __m256i status_byte =
        _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi32(bytes, 8), _mm256_set1_epi32(0xFF00)),
                        _mm256_set1_epi32(status << 8));
    const int data_mask = status == 0x90 ? 0xFFFF : 0x00FF;
    return _mm256_or_si256(
        status_byte, _mm256_slli_epi32(_mm256_and_si256(bytes, _mm256_set1_epi32(data_mask)), 16));
}

}

ScanResult scan_notes(const TrackView& view, const Unit& unit, std::uint64_t* local,
                      std::uint64_t* ahead) {
    const Note* const notes = view.notes;
    const std::uint64_t until = unit.off_until;
    const std::uint32_t track_end = view.track_end;
    // up to this tick a note off is the unit's whatever the position of its note. a unit
    // that ends inside the note offs of a tick has that tick left to the scalar code; the
    // tick is above zero then, a note off lies behind the start of its note
    const std::uint32_t until_tick = static_cast<std::uint32_t>(until >> 32);
    const std::uint32_t whole_ticks = static_cast<std::uint32_t>(until) == EVERY_NOTE
                                          ? until_tick
                                          : until_tick - (until_tick != 0 ? 1 : 0);
    const __m256i limit = _mm256_set1_epi32(static_cast<int>(whole_ticks));
    std::uint32_t prev = unit.on_begin > 0 ? notes[unit.on_begin - 1].start : 0;
    std::uint32_t disorder = 0;
    std::uint64_t overflow = 0;
    std::size_t local_count = 0;
    std::size_t ahead_count = 0;
    std::uint32_t i = unit.on_begin;
    while (i < unit.on_end) {
        if (unit.on_end - i >= 8) {
            // eight notes that are in order, end behind their start and inside the unit
            const Notes8 group = load_notes8(notes + i);
            const __m256i ends = _mm256_add_epi32(group.starts, group.lengths);
            const __m256i plain = _mm256_andnot_si256(
                not_above(ends, group.starts),
                _mm256_and_si256(not_above(ends, limit),
                                 not_above(shifted_in(group.starts, prev), group.starts)));
            if (lane_bits(plain) == 0xFF) [[likely]] {
                const __m256i words = event_words(group.bytes, 0x80);
                const __m256i low = _mm256_unpacklo_epi32(words, ends);
                const __m256i high = _mm256_unpackhi_epi32(words, ends);
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(local + local_count),
                                    _mm256_permute2x128_si256(low, high, 0x20));
                _mm256_storeu_si256(reinterpret_cast<__m256i*>(local + local_count + 4),
                                    _mm256_permute2x128_si256(low, high, 0x31));
                local_count += 8;
                prev = notes[i + 7].start;
                i += 8;
                continue;
            }
        }
        // both entries are written and one of them, or neither, is kept: no branches
        const std::uint32_t stop = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(unit.on_end, static_cast<std::uint64_t>(i) + SCALAR_STRETCH));
        for (; i < stop; ++i) {
            const Note& note = notes[i];
            const std::uint32_t start = note.start;
            disorder |= static_cast<std::uint32_t>(start < prev);
            prev = start;
            const std::uint64_t wide = static_cast<std::uint64_t>(start) + note.length;
            overflow |= wide >> 32;
            const std::uint32_t end =
                (wide >> 32) != 0 ? track_end : static_cast<std::uint32_t>(wide);
            const std::uint64_t key = off_key(end, i);
            local[local_count] = off_record(end, note);
            local_count += static_cast<std::size_t>((end != start) & (key <= until));
            ahead[ahead_count] = key;
            ahead_count += static_cast<std::size_t>((end != start) & (key > until));
        }
    }
    return ScanResult{local_count, ahead_count, disorder == 0, overflow != 0};
}

bool in_tick_order(const std::uint64_t* records, std::size_t count) {
    for (std::size_t i = 1; i < count; ++i) {
        if (record_tick(records[i]) < record_tick(records[i - 1])) {
            return false;
        }
    }
    return true;
}

std::uint64_t* sort_by_tick(std::uint64_t* records, std::uint64_t* spare, std::size_t count) {
    if (count <= SMALL_SORT) {
        for (std::size_t i = 1; i < count; ++i) {
            const std::uint64_t record = records[i];
            std::size_t at = i;
            while (at > 0 && record_tick(records[at - 1]) > record_tick(record)) {
                records[at] = records[at - 1];
                --at;
            }
            records[at] = record;
        }
        return records;
    }

    // counting passes over digits of equal width, lowest first. only the bits in which the
    // ticks differ from each other need sorting: the ticks of a unit lie close together.
    // found from the ticks themselves, not from where the plan says they lie: the sort has to
    // hold for a plan made from input that was not in order
    const std::uint32_t first = record_tick(records[0]);
    std::uint32_t differing = 0;
    for (std::size_t i = 0; i < count; ++i) {
        differing |= record_tick(records[i]) ^ first;
    }
    const unsigned bits = static_cast<unsigned>(std::bit_width(differing));
    if (bits == 0) {
        return records;
    }
    const unsigned passes = (bits + DIGIT_BITS - 1) / DIGIT_BITS;
    const unsigned digit_bits = (bits + passes - 1) / passes;
    const std::uint32_t mask = (1u << digit_bits) - 1;
    std::uint32_t counts[1u << DIGIT_BITS];
    for (unsigned pass = 0; pass < passes; ++pass) {
        const unsigned shift = pass * digit_bits;
        std::memset(counts, 0, sizeof(std::uint32_t) << digit_bits);
        for (std::size_t i = 0; i < count; ++i) {
            ++counts[(record_tick(records[i]) >> shift) & mask];
        }
        std::uint32_t total = 0;
        for (std::uint32_t d = 0; d <= mask; ++d) {
            const std::uint32_t n = counts[d];
            counts[d] = total;
            total += n;
        }
        for (std::size_t i = 0; i < count; ++i) {
            const std::uint64_t record = records[i];
            spare[counts[(record_tick(record) >> shift) & mask]++] = record;
        }
        std::swap(records, spare);
    }
    return records;
}

// a note off goes first when its tick is not behind the note on: every such note started
// earlier. channel events go behind the note events of their tick.
// runs of note offs and runs of note ons take turns. a run is written eight events at a time
// as long as every delta fits one byte, no channel event falls into it and, for note ons,
// every note ends behind its start; otherwise event by event
Cursor merge_events(Cursor start, const TrackView& view, const Unit& unit, const std::uint64_t* off,
                    std::size_t off_count) {
    constexpr std::uint64_t NEVER = 1ull << 32;
    const std::uint32_t track_end = view.track_end;
    const Note* note = view.notes + unit.on_begin;
    const Note* const note_stop = view.notes + unit.on_end;
    const std::uint64_t* const off_stop = off + off_count;
    const ChannelEvent* const channel_stop = view.channel + unit.ch_end;
    const ChannelEvent* channel = start.channel;
    std::uint64_t channel_tick = channel != channel_stop ? channel->tick : NEVER;
    std::uint8_t* out = start.out;
    std::uint32_t tick = start.tick;

    // the lambdas are forced inline: left as calls they keep out and tick in memory

    // word holds the three event bytes above an empty low byte
    const auto put = [&] [[msvc::forceinline]] (std::uint32_t at, std::uint32_t word) {
        if (channel_tick < at) [[unlikely]] {
            const Cursor cursor = put_channel_events(Cursor{out, tick, channel}, channel_stop, at);
            out = cursor.out;
            tick = cursor.tick;
            channel = cursor.channel;
            channel_tick = channel != channel_stop ? channel->tick : NEVER;
        }
        const std::uint32_t delta = at - tick;
        tick = at;
        if (delta < WIDE_DELTA) [[likely]] {
            store32(out, word | delta);
            out += 4;
        } else {
            out = put_vlq(out, delta);
            store32(out, word >> 8);
            out += 3;
        }
    };
    const auto put_on = [&] [[msvc::forceinline]] (const Note& on) {
        put(on.start, on_word(on));
        // a note that ends where it starts is closed right behind its note on. the note off is
        // written either way, to spare a branch: the next event overwrites it
        store32(out, static_cast<std::uint32_t>(off_record(0, on)));
        out += note_end(on, track_end) == on.start ? 4 : 0;
    };
    const auto put_offs_up_to = [&] [[msvc::forceinline]] (std::uint32_t last) {
        for (; off != off_stop && record_tick(*off) <= last; ++off) {
            put(record_tick(*off), static_cast<std::uint32_t>(*off));
        }
    };
    const auto put_ons_before = [&] [[msvc::forceinline]] (std::uint64_t limit) {
        for (; note != note_stop && note->start < limit; ++note) {
            put_on(*note);
        }
    };

    // writes the first count of eight events when their deltas allow it. the store runs over
    // into bytes that the events still to come fill
    const auto put8 = [&] [[msvc::forceinline]] (__m256i event_ticks, __m256i words, unsigned count,
                                                 unsigned usable) {
        const __m256i deltas = _mm256_sub_epi32(event_ticks, shifted_in(event_ticks, tick));
        usable &= lane_bits(not_above(deltas, _mm256_set1_epi32(static_cast<int>(WIDE_DELTA - 1))));
        // the tick of the last of them, or the current tick when there is none
        const __m256i picked = _mm256_permutevar8x32_epi32(
            event_ticks, _mm256_set1_epi32(static_cast<int>(count) - 1));
        const std::uint32_t last =
            count != 0
                ? static_cast<std::uint32_t>(_mm_cvtsi128_si32(_mm256_castsi256_si128(picked)))
                : tick;
        if ((~usable & ((1u << count) - 1)) != 0 || channel_tick < last) {
            return false;
        }
        _mm256_storeu_si256(reinterpret_cast<__m256i*>(out), _mm256_or_si256(words, deltas));
        out += 4 * count;
        tick = last;
        return true;
    };

    while (note_stop - note >= 8 && off_stop - off >= 8) {
        // note offs up to the next note on
        const std::uint32_t on_tick = note->start;
        const __m256i low = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(off));
        const __m256i high = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(off + 4));
        const __m256i odd = _mm256_castps_si256(_mm256_shuffle_ps(
            _mm256_castsi256_ps(low), _mm256_castsi256_ps(high), _MM_SHUFFLE(3, 1, 3, 1)));
        const __m256i even = _mm256_castps_si256(_mm256_shuffle_ps(
            _mm256_castsi256_ps(low), _mm256_castsi256_ps(high), _MM_SHUFFLE(2, 0, 2, 0)));
        const __m256i off_ticks = _mm256_permute4x64_epi64(odd, _MM_SHUFFLE(3, 1, 2, 0));
        const __m256i off_words = _mm256_permute4x64_epi64(even, _MM_SHUFFLE(3, 1, 2, 0));
        const unsigned offs = static_cast<unsigned>(std::countr_one(
            lane_bits(not_above(off_ticks, _mm256_set1_epi32(static_cast<int>(on_tick))))));
        if (put8(off_ticks, off_words, offs, 0xFF)) [[likely]] {
            off += offs;
            if (offs == 8) {
                continue;
            }
        } else {
            put_offs_up_to(on_tick);
            if (off_stop - off < 8) {
                break;
            }
        }

        // note ons before the next note off. its tick is above zero: the note has a length
        const std::uint32_t off_tick = record_tick(*off);
        const Notes8 group = load_notes8(note);
        const unsigned ons = static_cast<unsigned>(std::countr_one(
            lane_bits(not_above(group.starts, _mm256_set1_epi32(static_cast<int>(off_tick - 1))))));
        const unsigned with_length =
            ~lane_bits(not_above(_mm256_add_epi32(group.starts, group.lengths), group.starts));
        if (put8(group.starts, event_words(group.bytes, 0x90), ons, with_length)) [[likely]] {
            note += ons;
        } else {
            put_ons_before(off_tick);
        }
    }

    while (note != note_stop && off != off_stop) {
        put_offs_up_to(note->start);
        put_ons_before(off != off_stop ? record_tick(*off) : NEVER);
    }
    put_offs_up_to(LAST_TICK);
    put_ons_before(NEVER);
    return put_channel_events(Cursor{out, tick, channel}, channel_stop, NEVER);
}

bool channel_in_order(const TrackView& view, const Unit& unit) {
    std::uint32_t prev = unit.ch_begin > 0 ? view.channel[unit.ch_begin - 1].tick : 0;
    bool ordered = true;
    for (std::uint32_t i = unit.ch_begin; i < unit.ch_end; ++i) {
        ordered &= view.channel[i].tick >= prev;
        prev = view.channel[i].tick;
    }
    return ordered;
}

Extent measure(const TrackView& view, const Unit& unit, const std::uint64_t* offs,
               std::size_t off_count) {
    const std::size_t ons = unit.on_end - unit.on_begin;
    const std::size_t channel = unit.ch_end - unit.ch_begin;
    Extent extent;
    if (ons > 0) {
        extent.first = std::min(extent.first, view.notes[unit.on_begin].start);
        extent.last = std::max(extent.last, view.notes[unit.on_end - 1].start);
    }
    if (off_count > 0) {
        extent.first = std::min(extent.first, record_tick(offs[0]));
        extent.last = std::max(extent.last, record_tick(offs[off_count - 1]));
    }
    if (channel > 0) {
        extent.first = std::min(extent.first, view.channel[unit.ch_begin].tick);
        extent.last = std::max(extent.last, view.channel[unit.ch_end - 1].tick);
    }
    // every note counts twice: it may end where it starts. an event takes four bytes unless
    // its delta is wide, then up to eight, and the tick span limits how many of those there are
    const std::size_t events = 2 * ons + off_count + channel;
    const std::size_t wide =
        events > 0 ? std::min<std::size_t>(events, (extent.last - extent.first) / WIDE_DELTA) : 0;
    extent.bytes = 4 * events + 4 * wide;
    return extent;
}

std::uint64_t conductor_size(const Plan& plan) {
    std::uint64_t size = 4;
    std::uint32_t tick = 0;
    for (std::size_t i = 0; i < plan.metas->size(); ++i) {
        const MetaEvent& meta = plan.meta(i);
        size += vlq_length(meta.tick - tick) + 2 +
                vlq_length(static_cast<std::uint32_t>(meta.data.size())) + meta.data.size();
        tick = meta.tick;
    }
    return size;
}

std::uint8_t* put_prelude(const Plan& plan, std::uint8_t* out) {
    std::memcpy(out, "MThd\0\0\0\6\0\1", 10);
    const std::size_t chunks = plan.tracks.size() + 1;
    out[10] = static_cast<std::uint8_t>(chunks >> 8);
    out[11] = static_cast<std::uint8_t>(chunks);
    out[12] = static_cast<std::uint8_t>(plan.ppq >> 8);
    out[13] = static_cast<std::uint8_t>(plan.ppq);
    std::memcpy(out + 14, "MTrk", 4);
    store_be32(out + 18, static_cast<std::uint32_t>(plan.conductor_bytes));
    out += 22;

    std::uint32_t tick = 0;
    for (std::size_t i = 0; i < plan.metas->size(); ++i) {
        const MetaEvent& meta = plan.meta(i);
        out = put_vlq(out, meta.tick - tick);
        tick = meta.tick;
        *out++ = 0xFF;
        *out++ = static_cast<std::uint8_t>(meta.event_type);
        out = put_vlq(out, static_cast<std::uint32_t>(meta.data.size()));
        if (!meta.data.empty()) {
            std::memcpy(out, meta.data.data(), meta.data.size());
            out += meta.data.size();
        }
    }
    store32(out, END_OF_TRACK);
    return out + 4;
}

}
