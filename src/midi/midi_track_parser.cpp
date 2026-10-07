#include "midi/midi_track_parser.h"
#include "midi/fast_note_export.h"

#include <limits>
#include <bit>
#include <immintrin.h>
#if defined(_MSC_VER)
#include <stdlib.h>
#endif

#define SIMD_VLQ

namespace andromeda::midi {

namespace {

// 256 keys on purpose: rust gives keys >= 0x80 their own queue; do not mask
constexpr std::size_t SLOT_COUNT = 256 * 16;
constexpr MIDITick UNENDED = std::numeric_limits<MIDITick>::max();

constexpr std::ptrdiff_t TAIL_HEADROOM = 8;

// unchecked: callers must leave tail_headroom bytes; keep the 4-byte cap
inline MIDITick read_vlq(const std::uint8_t*& p) {
#ifndef SIMD_VLQ
    MIDITick v = *p++;
    if ((v & 0x80) != 0) {
        v &= 0x7F;
        for (int i = 0; i < 3; ++i) {
            const std::uint8_t c = *p++;
            v = (v << 7) | static_cast<MIDITick>(c & 0x7F);
            if ((c & 0x80) == 0) {
                break;
            }
        }
    }
    return v;
#else
    __m128i chunk = _mm_loadu_si128(reinterpret_cast<const __m128i*>(p));

    uint16_t mask = static_cast<uint32_t>(_mm_movemask_epi8(chunk));

    uint32_t end_mask = ~mask & 0xFFFF;
    int len = std::countr_zero(end_mask) + 1;

    const uint8_t* bytes = p;
    p += len;

    // 1-byte vlq
    if (len == 1) { return bytes[0]; }

    uint32_t raw = 0;
    std::memcpy(&raw, bytes, len <= 4 ? len : 4);

#if defined(_MSC_VER)
    raw = _byteswap_ulong(raw);
#else
    raw = std::byteswap(raw);
#endif

    constexpr uint32_t PEXT_MASK = 0x7F7F7F7F;
    return static_cast<MIDITick>(_pext_u32(raw, PEXT_MASK) >> ((4 - len) * 7));
#endif
}

inline MIDITick read_vlq_checked(const std::uint8_t*& p, const std::uint8_t* end) {
    if (p >= end) {
        return 0;
    }

    MIDITick v = *p++;
    if ((v & 0x80) != 0) {
        v &= 0x7F;
        for (int i = 0; i < 3 && p < end; ++i) {
            const std::uint8_t c = *p++;
            v = (v << 7) | static_cast<MIDITick>(c & 0x7F);
            if ((c & 0x80) == 0) {
                break;
            }
        }
    }
    return v;
}

}

TrackParseScratch::TrackParseScratch() : slots(SLOT_COUNT) {}

void TrackParseScratch::reset() {
    for (Pending& slot : slots) {
        slot.head = 0;
        slot.ids.clear();
    }
}

MIDITrackParser::MIDITrackParser(const std::uint8_t* data, std::size_t length)
    : begin_(data), end_(data + length) {}

void MIDITrackParser::parse_all(TrackParseScratch& scratch) {
    if (static_cast<std::size_t>(end_ - begin_) >= (1u << 20)) {
        run<false>(nullptr);
        note_events.reserve(static_cast<std::size_t>(counted_notes_));
        channel_events.reserve(static_cast<std::size_t>(counted_channel_evs_));
        parse_success = true;
        track_ended = false;
    } else {
        note_events.reserve(static_cast<std::size_t>(end_ - begin_) / 6);
    }

    scratch.reset();
    run<true>(&scratch);
}

template <bool Store>
void MIDITrackParser::run(TrackParseScratch* scratch) {
    const std::uint8_t* p = begin_;
    const std::uint8_t* const end = end_;

    MIDITick tick = 0;
    std::uint8_t running = 0;
    std::uint32_t next_note_id = 0;
    std::uint64_t note_count = 0;
    std::uint64_t channel_ev_count = 0;

    TrackParseScratch::Pending* const slots = Store ? scratch->slots.data() : nullptr;

    const auto close_note = [&](std::size_t slot_index, MIDITick end_tick) {
        TrackParseScratch::Pending& q = slots[slot_index];
        if (q.head >= q.ids.size()) {
            return;
        }

        const std::uint32_t id = q.ids[q.head++];
        if (q.head == q.ids.size()) {
            q.head = 0;
            q.ids.clear();
        }

        Note& note = note_events[id];
        note.length = end_tick - note.start;
    };

    const auto handle_system = [&](std::uint8_t status) -> bool {
        if (status == 0xFF) {
            if (p >= end) {
                return false;
            }

            const std::uint8_t meta_cmd = *p++;
            const MIDITick meta_len = read_vlq_checked(p, end);

            if (static_cast<std::size_t>(end - p) < meta_len) {
                parse_success = false;
                return false;
            }

            const std::uint8_t* const meta_data = p;
            p += meta_len;

            const auto push_meta = [&](MetaEventType type) {
                if constexpr (Store) {
                    meta_events.push_back(
                        MetaEvent{.tick = tick,
                                  .event_type = type,
                                  .data = std::vector<std::uint8_t>(meta_data, meta_data + meta_len)});
                }
            };

            switch (meta_cmd) {
            case 0x00:
                if (meta_len != 0x00 && meta_len != 0x02) {
                    parse_success = false;
                    return true;
                }
                push_meta(MetaEventType::SequenceNumber);
                break;
            case 0x01: push_meta(MetaEventType::Text); break;
            case 0x02: push_meta(MetaEventType::Copyright); break;
            case 0x03: push_meta(MetaEventType::TrackName); break;
            case 0x04: push_meta(MetaEventType::InstrumentName); break;
            case 0x05: push_meta(MetaEventType::Lyric); break;
            case 0x06: push_meta(MetaEventType::Marker); break;
            case 0x07: push_meta(MetaEventType::CuePoint); break;
            case 0x08: push_meta(MetaEventType::ProgramName); break;
            case 0x09: push_meta(MetaEventType::DeviceName); break;
            case 0x20:
                if (meta_len != 0x01) {
                    parse_success = false;
                    return true;
                }
                push_meta(MetaEventType::ChannelPrefix);
                break;
            case 0x21:
                if (meta_len != 0x01) {
                    parse_success = false;
                    return true;
                }
                push_meta(MetaEventType::MIDIPort);
                break;
            case 0x2F:
                track_ended = true;
                if (meta_len != 0x00) {
                    parse_success = false;
                }
                return false;
            case 0x51:
                if (meta_len != 0x03) {
                    parse_success = false;
                    return true;
                }
                push_meta(MetaEventType::Tempo);
                break;
            case 0x54:
                if (meta_len != 0x05) {
                    parse_success = false;
                    return true;
                }
                push_meta(MetaEventType::SMPTEOffset);
                break;
            case 0x58:
                if (meta_len != 0x04) {
                    parse_success = false;
                    return true;
                }
                push_meta(MetaEventType::TimeSignature);
                break;
            case 0x59:
                if (meta_len != 0x02) {
                    parse_success = false;
                    return true;
                }
                push_meta(MetaEventType::KeySignature);
                break;
            default: break;
            }

            return true;
        }

        if (status == 0xF0 || status == 0xF7) {
            const MIDITick sysex_len = read_vlq_checked(p, end);
            if (static_cast<std::size_t>(end - p) < sysex_len) {
                parse_success = false;
                return false;
            }
            p += sysex_len;
            return true;
        }

        if (status == 0xF2) {
            if (end - p < 2) {
                return false;
            }
            p += 2;
            return true;
        }

        if (status == 0xF3) {
            if (end - p < 1) {
                return false;
            }
            p += 1;
            return true;
        }

        return false;
    };

    const std::uint8_t* const bulk_end =
        (end - begin_ >= TAIL_HEADROOM) ? end - TAIL_HEADROOM : begin_;

    while (p < bulk_end) {
        tick += read_vlq(p);

        std::uint8_t status = *p;
        if (status < 0x80) {
            if (running == 0) {
                parse_success = false;
                goto finish;
            }
            status = running;
        } else {
            ++p;
            if (status >= 0xF0) {
                if (!handle_system(status)) {
                    goto finish;
                }
                continue;
            }
            running = status;
        }

        {
            const std::uint8_t channel = status & 0x0F;

            switch (status >> 4) {
            case 0x8: {
                const std::uint8_t key = p[0];
                p += 2;
                if constexpr (Store) {
                    close_note((static_cast<std::size_t>(key) << 4) | channel, tick);
                }
                break;
            }
            case 0x9: {
                const std::uint8_t key = p[0];
                const std::uint8_t velocity = p[1];
                p += 2;

                if (velocity > 0) {
                    if constexpr (Store) {
                        note_events.push_back(Note{.start = tick,
                                                   .length = UNENDED,
                                                   .key = key,
                                                   .velocity = velocity,
                                                   .channel = channel});
                        TrackParseScratch::Pending& q =
                            slots[(static_cast<std::size_t>(key) << 4) | channel];
                        q.ids.push_back(next_note_id++);
                    } else {
                        ++note_count;
                    }
                } else if constexpr (Store) {
                    close_note((static_cast<std::size_t>(key) << 4) | channel, tick);
                }
                break;
            }
            case 0xA: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = NoteAftertouch{p[0], p[1]}});
                } else {
                    ++channel_ev_count;
                }
                p += 2;
                break;
            }
            case 0xB: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = Controller{p[0], p[1]}});
                } else {
                    ++channel_ev_count;
                }
                p += 2;
                break;
            }
            case 0xC: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = ProgramChange{p[0]}});
                } else {
                    ++channel_ev_count;
                }
                p += 1;
                break;
            }
            case 0xD: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = ChannelAftertouch{p[0]}});
                } else {
                    ++channel_ev_count;
                }
                p += 1;
                break;
            }
            case 0xE: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = PitchBend{p[0], p[1]}});
                } else {
                    ++channel_ev_count;
                }
                p += 2;
                break;
            }
            default: goto finish;
            }
        }
    }

    while (p < end) {
        tick += read_vlq_checked(p, end);
        if (p >= end) {
            break;
        }

        std::uint8_t status = *p;
        if (status < 0x80) {
            if (running == 0) {
                parse_success = false;
                goto finish;
            }
            status = running;
        } else {
            ++p;
            if (status >= 0xF0) {
                if (!handle_system(status)) {
                    goto finish;
                }
                continue;
            }
            running = status;
        }

        {
            const std::uint8_t channel = status & 0x0F;
            const std::uint8_t hi = status >> 4;
            const std::ptrdiff_t needed = (hi == 0xC || hi == 0xD) ? 1 : 2;
            if (end - p < needed) {
                goto finish;
            }

            switch (hi) {
            case 0x8: {
                const std::uint8_t key = p[0];
                p += 2;
                if constexpr (Store) {
                    close_note((static_cast<std::size_t>(key) << 4) | channel, tick);
                }
                break;
            }
            case 0x9: {
                const std::uint8_t key = p[0];
                const std::uint8_t velocity = p[1];
                p += 2;

                if (velocity > 0) {
                    if constexpr (Store) {
                        note_events.push_back(Note{.start = tick,
                                                   .length = UNENDED,
                                                   .key = key,
                                                   .velocity = velocity,
                                                   .channel = channel});
                        slots[(static_cast<std::size_t>(key) << 4) | channel].ids.push_back(
                            next_note_id++);
                    } else {
                        ++note_count;
                    }
                } else if constexpr (Store) {
                    close_note((static_cast<std::size_t>(key) << 4) | channel, tick);
                }
                break;
            }
            case 0xA: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = NoteAftertouch{p[0], p[1]}});
                } else {
                    ++channel_ev_count;
                }
                p += 2;
                break;
            }
            case 0xB: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = Controller{p[0], p[1]}});
                } else {
                    ++channel_ev_count;
                }
                p += 2;
                break;
            }
            case 0xC: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = ProgramChange{p[0]}});
                } else {
                    ++channel_ev_count;
                }
                p += 1;
                break;
            }
            case 0xD: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = ChannelAftertouch{p[0]}});
                } else {
                    ++channel_ev_count;
                }
                p += 1;
                break;
            }
            case 0xE: {
                if constexpr (Store) {
                    channel_events.push_back(ChannelEvent{
                        .tick = tick, .channel = channel, .event_type = PitchBend{p[0], p[1]}});
                } else {
                    ++channel_ev_count;
                }
                p += 2;
                break;
            }
            default: goto finish;
            }
        }
    }

finish:
    track_ended = true;

    if constexpr (!Store) {
        counted_notes_ = note_count;
        counted_channel_evs_ = channel_ev_count;
    }
}

template void MIDITrackParser::run<false>(TrackParseScratch*);
template void MIDITrackParser::run<true>(TrackParseScratch*);

}
