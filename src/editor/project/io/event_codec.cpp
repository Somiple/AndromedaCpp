#include "event_codec.h"

#include <algorithm>
#include <functional>
#include <type_traits>

#include <zstd.h>

namespace andromeda::editor::codec {

namespace {

constexpr int ZSTD_LEVEL = 9;

// Stable on-disk ids. Do NOT derive these from the variant index, so reordering
// ChannelEventType in the source never silently breaks old project files.
enum class ChannelTag : std::uint8_t {
    NoteOff = 0,
    NoteOn = 1,
    NoteAftertouch = 2,
    Controller = 3,
    ProgramChange = 4,
    ChannelAftertouch = 5,
    PitchBend = 6,
};

// ---- VLQ -------------------------------------------------------------------

void put_vlq(Bytes& out, std::uint64_t value) {
    std::uint8_t tmp[10];
    int n = 0;

    tmp[n++] = static_cast<std::uint8_t>(value & 0x7F);
    value >>= 7;
    while (value != 0) {
        tmp[n++] = static_cast<std::uint8_t>((value & 0x7F) | 0x80);
        value >>= 7;
    }
    while (n > 0) {
        out.push_back(tmp[--n]);
    }
}

// Reads never go out of bounds. On any failure the cursor latches `ok() == false`
// and returns zeros, so decoders can read a whole entry and check once.
class Cursor {
public:
    explicit Cursor(std::span<const std::uint8_t> data) : data_(data) {}

    [[nodiscard]] bool ok() const { return ok_; }
    [[nodiscard]] bool at_end() const { return pos_ >= data_.size(); }

    std::uint8_t u8() {
        if (pos_ >= data_.size()) {
            ok_ = false;
            return 0;
        }
        return data_[pos_++];
    }

    std::uint64_t vlq() {
        std::uint64_t value = 0;
        for (int i = 0; i < 10; ++i) {
            const std::uint8_t b = u8();
            if (!ok_) return 0;
            if ((value >> 57) != 0) {  // next shift would overflow 64 bits
                ok_ = false;
                return 0;
            }
            value = (value << 7) | (b & 0x7Fu);
            if ((b & 0x80u) == 0) return value;
        }
        ok_ = false;  // more than 10 groups
        return 0;
    }

    std::span<const std::uint8_t> take(std::size_t n) {
        if (n > data_.size() - pos_) {
            ok_ = false;
            return {};
        }
        const auto s = data_.subspan(pos_, n);
        pos_ += n;
        return s;
    }

private:
    std::span<const std::uint8_t> data_;
    std::size_t pos_ = 0;
    bool ok_ = true;
};

// Calls f(item) for every item in tick order, sorting a copy only if needed.
template <typename T, typename TickProj, typename F>
void for_each_sorted(std::span<const T> items, TickProj tick_of, F&& f) {
    if (std::ranges::is_sorted(items, std::ranges::less{}, tick_of)) {
        for (const T& item : items) f(item);
        return;
    }
    std::vector<T> copy(items.begin(), items.end());
    std::ranges::stable_sort(copy, std::ranges::less{}, tick_of);
    for (const T& item : copy) f(item);
}

}  // namespace

// ============================================================================
// Notes
// ============================================================================

Bytes encode_notes(std::span<const midi::Note> notes) {
    Bytes out;
    out.reserve(notes.size() * 6);

    std::uint64_t prev = 0;
    for_each_sorted(
        notes, [](const midi::Note& n) { return n.start; },
        [&](const midi::Note& n) {
            const auto start = static_cast<std::uint64_t>(n.start);
            put_vlq(out, start - prev);
            prev = start;

            put_vlq(out, static_cast<std::uint64_t>(n.length));
            out.push_back(n.key);
            out.push_back(n.velocity);
            out.push_back(n.channel);
        });

    return out;
}

Result<std::vector<midi::Note>> decode_notes(std::span<const std::uint8_t> bytes,
                                             std::optional<std::size_t> expected_count) {
    std::vector<midi::Note> notes;
    if (expected_count) {
        notes.reserve(std::min(*expected_count, bytes.size() / 5));  // 5 = min bytes per note
    }

    Cursor c(bytes);
    std::uint64_t tick = 0;

    while (!c.at_end()) {
        tick += c.vlq();

        midi::Note n;
        n.start = static_cast<MIDITick>(tick);
        n.length = static_cast<MIDITick>(c.vlq());
        n.key = c.u8();
        n.velocity = c.u8();
        n.channel = c.u8();

        if (!c.ok()) return std::unexpected("Corrupt note data: truncated or malformed entry");
        notes.push_back(n);
    }

    if (expected_count && notes.size() != *expected_count) {
        return std::unexpected("Corrupt note data: note count does not match track metadata");
    }
    return notes;
}

// ============================================================================
// Channel events
// ============================================================================

Bytes encode_channel_events(std::span<const midi::ChannelEvent> events) {
    Bytes out;
    out.reserve(events.size() * 4);

    std::uint64_t prev = 0;
    for_each_sorted(
        events, [](const midi::ChannelEvent& e) { return e.tick; },
        [&](const midi::ChannelEvent& e) {
            const auto tick = static_cast<std::uint64_t>(e.tick);
            put_vlq(out, tick - prev);
            prev = tick;

            const auto head = [&](ChannelTag tag) {
                out.push_back(static_cast<std::uint8_t>((static_cast<std::uint8_t>(tag) << 4) |
                                                        (e.channel & 0x0F)));
            };

            std::visit(
                [&](const auto& ev) {
                    using T = std::decay_t<decltype(ev)>;
                    if constexpr (std::is_same_v<T, midi::NoteOff>) {
                        head(ChannelTag::NoteOff);
                        out.push_back(ev.key);
                    } else if constexpr (std::is_same_v<T, midi::NoteOn>) {
                        head(ChannelTag::NoteOn);
                        out.push_back(ev.key);
                        out.push_back(ev.velocity);
                    } else if constexpr (std::is_same_v<T, midi::NoteAftertouch>) {
                        head(ChannelTag::NoteAftertouch);
                        out.push_back(ev.key);
                        out.push_back(ev.pressure);
                    } else if constexpr (std::is_same_v<T, midi::Controller>) {
                        head(ChannelTag::Controller);
                        out.push_back(ev.controller);
                        out.push_back(ev.value);
                    } else if constexpr (std::is_same_v<T, midi::ProgramChange>) {
                        head(ChannelTag::ProgramChange);
                        out.push_back(ev.program);
                    } else if constexpr (std::is_same_v<T, midi::ChannelAftertouch>) {
                        head(ChannelTag::ChannelAftertouch);
                        out.push_back(ev.amount);
                    } else if constexpr (std::is_same_v<T, midi::PitchBend>) {
                        head(ChannelTag::PitchBend);
                        out.push_back(ev.lsb);
                        out.push_back(ev.msb);
                    } else {
                        static_assert(!sizeof(T), "unhandled channel event type in encode_channel_events");
                    }
                },
                e.event_type);
        });

    return out;
}

Result<std::vector<midi::ChannelEvent>> decode_channel_events(
    std::span<const std::uint8_t> bytes, std::optional<std::size_t> expected_count) {
    std::vector<midi::ChannelEvent> events;
    if (expected_count) {
        events.reserve(std::min(*expected_count, bytes.size() / 3));  // 3 = min bytes per event
    }

    Cursor c(bytes);
    std::uint64_t tick = 0;

    while (!c.at_end()) {
        tick += c.vlq();

        const std::uint8_t head = c.u8();
        const auto tag = static_cast<ChannelTag>(head >> 4);

        midi::ChannelEventType payload{};
        switch (tag) {
        case ChannelTag::NoteOff:
            payload = midi::NoteOff{c.u8()};
            break;
        case ChannelTag::NoteOn: {
            const auto key = c.u8();
            const auto vel = c.u8();
            payload = midi::NoteOn{key, vel};
            break;
        }
        case ChannelTag::NoteAftertouch: {
            const auto key = c.u8();
            const auto pressure = c.u8();
            payload = midi::NoteAftertouch{key, pressure};
            break;
        }
        case ChannelTag::Controller: {
            const auto controller = c.u8();
            const auto value = c.u8();
            payload = midi::Controller{controller, value};
            break;
        }
        case ChannelTag::ProgramChange:
            payload = midi::ProgramChange{c.u8()};
            break;
        case ChannelTag::ChannelAftertouch:
            payload = midi::ChannelAftertouch{c.u8()};
            break;
        case ChannelTag::PitchBend: {
            const auto lsb = c.u8();
            const auto msb = c.u8();
            payload = midi::PitchBend{lsb, msb};
            break;
        }
        default:
            return std::unexpected("Corrupt channel event data: unknown event tag");
        }

        if (!c.ok()) {
            return std::unexpected("Corrupt channel event data: truncated or malformed entry");
        }

        midi::ChannelEvent ev;
        ev.tick = static_cast<MIDITick>(tick);
        ev.channel = static_cast<std::uint8_t>(head & 0x0F);
        ev.event_type = payload;
        events.push_back(ev);
    }

    if (expected_count && events.size() != *expected_count) {
        return std::unexpected("Corrupt channel event data: count does not match track metadata");
    }
    return events;
}

// ============================================================================
// Global (meta) events
// ============================================================================

Bytes encode_meta_events(std::span<const midi::MetaEvent> events) {
    Bytes out;
    out.reserve(events.size() * 8);

    std::uint64_t prev = 0;
    for_each_sorted(
        events, [](const midi::MetaEvent& e) { return e.tick; },
        [&](const midi::MetaEvent& e) {
            const auto tick = static_cast<std::uint64_t>(e.tick);
            put_vlq(out, tick - prev);
            prev = tick;

            out.push_back(static_cast<std::uint8_t>(e.event_type));
            put_vlq(out, e.data.size());
            out.insert(out.end(), e.data.begin(), e.data.end());
        });

    return out;
}

Result<std::vector<midi::MetaEvent>> decode_meta_events(std::span<const std::uint8_t> bytes,
                                                        std::optional<std::size_t> expected_count) {
    std::vector<midi::MetaEvent> events;
    if (expected_count) {
        events.reserve(std::min(*expected_count, bytes.size() / 3));  // 3 = min bytes per event
    }

    Cursor c(bytes);
    std::uint64_t tick = 0;

    while (!c.at_end()) {
        tick += c.vlq();

        midi::MetaEvent ev;
        ev.tick = static_cast<MIDITick>(tick);
        ev.event_type = static_cast<midi::MetaEventType>(c.u8());

        const std::uint64_t size = c.vlq();
        if (!c.ok() || size > bytes.size()) {
            return std::unexpected("Corrupt global event data: bad data size");
        }
        const auto data = c.take(static_cast<std::size_t>(size));
        if (!c.ok()) {
            return std::unexpected("Corrupt global event data: truncated entry");
        }

        ev.data.assign(data.begin(), data.end());
        events.push_back(std::move(ev));
    }

    if (expected_count && events.size() != *expected_count) {
        return std::unexpected("Corrupt global event data: count mismatch");
    }
    return events;
}

// ============================================================================
// Compression
// ============================================================================

Result<Bytes> compress(std::span<const std::uint8_t> raw) {
    if (raw.empty()) return Bytes{};

    Bytes out(ZSTD_compressBound(raw.size()));
    const std::size_t n = ZSTD_compress(out.data(), out.size(), raw.data(), raw.size(), ZSTD_LEVEL);
    if (ZSTD_isError(n)) {
        return std::unexpected(std::string("Compression failed: ") + ZSTD_getErrorName(n));
    }

    out.resize(n);
    return out;
}

Result<Bytes> decompress(std::span<const std::uint8_t> compressed, std::size_t raw_size) {
    if (raw_size == 0) return Bytes{};
    if (raw_size > MAX_BLOCK_SIZE) {
        return std::unexpected("Corrupt block: uncompressed size is unreasonably large");
    }

    Bytes out(raw_size);
    const std::size_t n = ZSTD_decompress(out.data(), out.size(), compressed.data(), compressed.size());
    if (ZSTD_isError(n)) {
        return std::unexpected(std::string("Decompression failed: ") + ZSTD_getErrorName(n));
    }
    if (n != raw_size) {
        return std::unexpected("Corrupt block: decompressed size does not match header");
    }

    return out;
}

}
