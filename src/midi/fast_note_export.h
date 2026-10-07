#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "editor/midi_types.h"
#include "midi/events/channel_event.h"
#include "midi/events/note.h"

namespace andromeda::midi::fast_export {

using Tick = andromeda::editor::MIDITick;
static_assert(std::is_unsigned_v<Tick> && sizeof(Tick) == 4,
              "fast path packs ticks into 32 bits; the original VLQ buffer (5 bytes) implies this too");

[[nodiscard]] inline std::size_t vlq_len(std::uint32_t v) {
    return 1 + (v >= 0x80u) + (v >= 0x4000u) + (v >= 0x200000u) + (v >= 0x10000000u);
}

inline std::uint8_t* put_vlq(std::uint8_t* p, std::uint32_t v) {
    if (v < 0x80u) {
        *p++ = static_cast<std::uint8_t>(v);
    } else if (v < 0x4000u) {
        *p++ = static_cast<std::uint8_t>((v >> 7) | 0x80u);
        *p++ = static_cast<std::uint8_t>(v & 0x7Fu);
    } else if (v < 0x200000u) {
        *p++ = static_cast<std::uint8_t>((v >> 14) | 0x80u);
        *p++ = static_cast<std::uint8_t>(((v >> 7) & 0x7Fu) | 0x80u);
        *p++ = static_cast<std::uint8_t>(v & 0x7Fu);
    } else if (v < 0x10000000u) {
        *p++ = static_cast<std::uint8_t>((v >> 21) | 0x80u);
        *p++ = static_cast<std::uint8_t>(((v >> 14) & 0x7Fu) | 0x80u);
        *p++ = static_cast<std::uint8_t>(((v >> 7) & 0x7Fu) | 0x80u);
        *p++ = static_cast<std::uint8_t>(v & 0x7Fu);
    } else {
        *p++ = static_cast<std::uint8_t>((v >> 28) | 0x80u);
        *p++ = static_cast<std::uint8_t>(((v >> 21) & 0x7Fu) | 0x80u);
        *p++ = static_cast<std::uint8_t>(((v >> 14) & 0x7Fu) | 0x80u);
        *p++ = static_cast<std::uint8_t>(((v >> 7) & 0x7Fu) | 0x80u);
        *p++ = static_cast<std::uint8_t>(v & 0x7Fu);
    }
    return p;
}

namespace detail {

struct Rec {
    std::uint32_t tick;
    std::uint32_t bytes;  // status | data1 << 8 | data2 << 16 | kPairFlag
};

constexpr std::uint32_t kPairFlag = 1u << 24;   // on-record of a zero-length note: also emit its off
constexpr std::uint32_t kShortFlag = 1u << 25;  // 2-byte channel event (program change, ch. pressure)
constexpr std::size_t kChunkRecords = std::size_t{1} << 20;

inline std::uint32_t pack_channel_event(const ChannelEvent& ev) {
    const std::uint32_t ch = ev.channel & 0x0Fu;
    return std::visit(
        [ch](const auto& e) -> std::uint32_t {
            using T = std::decay_t<decltype(e)>;
            if constexpr (std::is_same_v<T, NoteOff>) {
                return (0x80u | ch) | (std::uint32_t{e.key} << 8);
            } else if constexpr (std::is_same_v<T, NoteOn>) {
                return (0x90u | ch) | (std::uint32_t{e.key} << 8) | (std::uint32_t{e.velocity} << 16);
            } else if constexpr (std::is_same_v<T, NoteAftertouch>) {
                return (0xA0u | ch) | (std::uint32_t{e.key} << 8) | (std::uint32_t{e.pressure} << 16);
            } else if constexpr (std::is_same_v<T, Controller>) {
                return (0xB0u | ch) | (std::uint32_t{e.controller} << 8) | (std::uint32_t{e.value} << 16);
            } else if constexpr (std::is_same_v<T, ProgramChange>) {
                return (0xC0u | ch) | (std::uint32_t{e.program} << 8) | kShortFlag;
            } else if constexpr (std::is_same_v<T, ChannelAftertouch>) {
                return (0xD0u | ch) | (std::uint32_t{e.amount} << 8) | kShortFlag;
            } else {
                static_assert(std::is_same_v<T, PitchBend>);
                return (0xE0u | ch) | (std::uint32_t{e.lsb} << 8) | (std::uint32_t{e.msb} << 16);
            }
        },
        ev.event_type);
}

template <class F>
void run_threads(unsigned threads, F&& f) {
    if (threads <= 1) {
        f(0u);
        return;
    }
    std::vector<std::jthread> workers;
    workers.reserve(threads - 1);
    for (unsigned t = 1; t < threads; ++t) {
        workers.emplace_back([&f, t] { f(t); });
    }
    f(0u);
}

inline std::pair<std::size_t, std::size_t> slice(std::size_t n, unsigned t, unsigned threads) {
    return {n * t / threads, n * (t + 1) / threads};
}

inline unsigned effective_threads(unsigned wanted, std::size_t n) {
    const std::size_t by_size = std::max<std::size_t>(1, n >> 16);  // >= 64K items per thread
    return static_cast<unsigned>(std::min<std::size_t>(wanted, by_size));
}

// Stable LSD radix sort by tick. Returns whichever of a/tmp holds the result.
// Skips the whole sort if already sorted, and skips byte-passes where all keys agree.
inline Rec* radix_sort_by_tick(Rec* a, Rec* tmp, std::size_t n, unsigned wanted_threads) {
    if (n < 2) {
        return a;
    }
    const unsigned threads = effective_threads(wanted_threads, n);

    std::vector<std::uint32_t> diff(threads, 0);
    std::vector<char> sorted(threads, 1);
    const std::uint32_t first = a[0].tick;

    run_threads(threads, [&](unsigned t) {
        const auto [lo, hi] = slice(n, t, threads);
        std::uint32_t d = 0;
        bool s = true;
        for (std::size_t i = lo; i < hi; ++i) {
            d |= a[i].tick ^ first;
            if (i > 0 && a[i - 1].tick > a[i].tick) {
                s = false;
            }
        }
        diff[t] = d;
        sorted[t] = s;
    });

    std::uint32_t all_diff = 0;
    bool all_sorted = true;
    for (unsigned t = 0; t < threads; ++t) {
        all_diff |= diff[t];
        all_sorted = all_sorted && sorted[t];
    }
    if (all_sorted) {
        return a;
    }

    std::vector<std::size_t> cnt(static_cast<std::size_t>(threads) * 256);
    Rec* src = a;
    Rec* dst = tmp;

    for (unsigned shift = 0; shift < 32; shift += 8) {
        if (((all_diff >> shift) & 0xFFu) == 0) {
            continue;
        }

        run_threads(threads, [&](unsigned t) {
            std::size_t* h = &cnt[static_cast<std::size_t>(t) * 256];
            std::fill(h, h + 256, std::size_t{0});
            const auto [lo, hi] = slice(n, t, threads);
            for (std::size_t i = lo; i < hi; ++i) {
                ++h[(src[i].tick >> shift) & 0xFFu];
            }
        });

        std::size_t sum = 0;
        for (unsigned b = 0; b < 256; ++b) {
            for (unsigned t = 0; t < threads; ++t) {
                const std::size_t c = cnt[static_cast<std::size_t>(t) * 256 + b];
                cnt[static_cast<std::size_t>(t) * 256 + b] = sum;
                sum += c;
            }
        }

        run_threads(threads, [&](unsigned t) {
            std::size_t* off = &cnt[static_cast<std::size_t>(t) * 256];
            const auto [lo, hi] = slice(n, t, threads);
            for (std::size_t i = lo; i < hi; ++i) {
                const Rec r = src[i];
                dst[off[(r.tick >> shift) & 0xFFu]++] = r;
            }
        });

        std::swap(src, dst);
    }
    return src;
}

// Merge order: off B[j] precedes on A[i] iff B[j].tick <= A[i].tick.
// Returns i such that the first k merged records are exactly A[0,i) + B[0,k-i).
inline std::size_t corank(const Rec* A, std::size_t na, const Rec* B, std::size_t nb, std::size_t k) {
    std::size_t lo = k > nb ? k - nb : 0;
    std::size_t hi = std::min(k, na);
    while (lo < hi) {
        const std::size_t i = lo + (hi - lo) / 2;
        const std::size_t j = k - i;  // >= 1 because i < hi <= k
        if (B[j - 1].tick > A[i].tick) {
            lo = i + 1;
        } else {
            hi = i;
        }
    }
    return lo;
}

template <class Emit>
inline void merge_range(const Rec* A, std::size_t i, std::size_t i1, const Rec* B, std::size_t j,
                        std::size_t j1, std::uint32_t prev, Emit&& emit) {
    while (i < i1 || j < j1) {
        if (j < j1 && (i >= i1 || B[j].tick <= A[i].tick)) {
            emit(B[j].tick - prev, B[j].bytes & (0xFFFFFFu | kShortFlag));
            prev = B[j].tick;
            ++j;
        } else {
            const Rec r = A[i];
            emit(r.tick - prev, r.bytes & (0xFFFFFFu | kShortFlag));
            prev = r.tick;
            if (r.bytes & kPairFlag) {
                emit(0u, (r.bytes & 0xFFFFu) ^ 0x10u);  // 0x9n -> 0x8n, velocity 0
            }
            ++i;
        }
    }
}

}  // namespace detail

// Serializes notes (+ optional channel events) as (delta VLQ + 2/3-byte channel event)
// records. The returned blocks must be written back-to-back, in order. Every block holds
// whole events.
[[nodiscard]] inline std::vector<std::vector<std::uint8_t>> encode_notes(
    const std::vector<Note>& notes, const std::vector<ChannelEvent>& channel_events,
    unsigned threads = 0) {
    using detail::Rec;

    const std::size_t n = notes.size();
    const std::size_t c = channel_events.size();
    const std::size_t na = n + c;  // "A" stream: note-ons first, then channel events
    if (na == 0) {
        return {};
    }
    if (threads == 0) {
        threads = std::max(1u, std::thread::hardware_concurrency());
    }
    const unsigned T = detail::effective_threads(threads, na);

    const auto end_of = [](const Note& nt) {
        return static_cast<std::uint32_t>(nt.get_start() + nt.get_length());
    };

    // 1a. on records (+ count of notes that need a separate off record)
    std::unique_ptr<Rec[]> ons(new Rec[na]);
    std::vector<std::size_t> nonzero(T, 0);
    detail::run_threads(T, [&](unsigned t) {
        const auto [lo, hi] = detail::slice(n, t, T);
        std::size_t nz = 0;
        for (std::size_t i = lo; i < hi; ++i) {
            const Note& nt = notes[i];
            const std::uint32_t s = static_cast<std::uint32_t>(nt.get_start());
            const bool zero_len = end_of(nt) == s;
            nz += !zero_len;
            ons[i].tick = s;
            ons[i].bytes = (0x90u | (static_cast<std::uint32_t>(nt.get_channel()) & 0x0Fu)) |
                           (static_cast<std::uint32_t>(nt.get_key()) << 8) |
                           (static_cast<std::uint32_t>(nt.get_velocity()) << 16) |
                           (zero_len ? detail::kPairFlag : 0u);
        }
        nonzero[t] = nz;
    });

    // 1a'. channel events go after the notes: a stable sort keeps them behind note-ons that
    // share their tick, which is what merge_events did (note stream wins ties).
    detail::run_threads(T, [&](unsigned t) {
        const auto [lo, hi] = detail::slice(c, t, T);
        for (std::size_t i = lo; i < hi; ++i) {
            const ChannelEvent& ev = channel_events[i];
            ons[n + i].tick = static_cast<std::uint32_t>(ev.tick);
            ons[n + i].bytes = detail::pack_channel_event(ev);
        }
    });

    std::vector<std::size_t> base(T + 1, 0);
    for (unsigned t = 0; t < T; ++t) {
        base[t + 1] = base[t] + nonzero[t];
    }
    const std::size_t nb = base[T];

    // 1b. off records
    std::unique_ptr<Rec[]> offs(new Rec[std::max<std::size_t>(nb, 1)]);
    detail::run_threads(T, [&](unsigned t) {
        const auto [lo, hi] = detail::slice(n, t, T);
        std::size_t w = base[t];
        for (std::size_t i = lo; i < hi; ++i) {
            const Note& nt = notes[i];
            const std::uint32_t e = end_of(nt);
            if (e != static_cast<std::uint32_t>(nt.get_start())) {
                offs[w].tick = e;
                offs[w].bytes = (0x80u | (static_cast<std::uint32_t>(nt.get_channel()) & 0x0Fu)) |
                                (static_cast<std::uint32_t>(nt.get_key()) << 8);
                ++w;
            }
        }
    });

    // 2. sort both streams
    std::unique_ptr<Rec[]> scratch(new Rec[na]);
    const Rec* A = detail::radix_sort_by_tick(ons.get(), scratch.get(), na, threads);
    Rec* free_buf = (A == ons.get()) ? scratch.get() : ons.get();
    const Rec* B = detail::radix_sort_by_tick(offs.get(), free_buf, nb, threads);

    // 3. chunked parallel merge + encode
    const std::size_t total = na + nb;
    const std::size_t chunks = (total + detail::kChunkRecords - 1) / detail::kChunkRecords;
    std::vector<std::vector<std::uint8_t>> blocks(chunks);
    std::atomic<std::size_t> next{0};

    const auto worker = [&] {
        for (;;) {
            const std::size_t ck = next.fetch_add(1, std::memory_order_relaxed);
            if (ck >= chunks) {
                return;
            }
            const std::size_t k0 = ck * detail::kChunkRecords;
            const std::size_t k1 = std::min(total, k0 + detail::kChunkRecords);
            const std::size_t i0 = detail::corank(A, na, B, nb, k0);
            const std::size_t i1 = detail::corank(A, na, B, nb, k1);
            const std::size_t j0 = k0 - i0;
            const std::size_t j1 = k1 - i1;

            std::uint32_t prev = 0;
            if (k0 > 0) {
                if (i0 > 0) prev = std::max(prev, A[i0 - 1].tick);
                if (j0 > 0) prev = std::max(prev, B[j0 - 1].tick);
            }

            std::size_t bytes = 0;
            detail::merge_range(A, i0, i1, B, j0, j1, prev, [&](std::uint32_t d, std::uint32_t b) {
                bytes += vlq_len(d) + 3 - ((b >> 25) & 1u);
            });

            std::vector<std::uint8_t>& blk = blocks[ck];
            blk.resize(bytes);
            std::uint8_t* p = blk.data();
            detail::merge_range(A, i0, i1, B, j0, j1, prev, [&](std::uint32_t d, std::uint32_t b) {
                p = put_vlq(p, d);
                p[0] = static_cast<std::uint8_t>(b);
                p[1] = static_cast<std::uint8_t>(b >> 8);
                if (b & detail::kShortFlag) {
                    p += 2;
                } else {
                    p[2] = static_cast<std::uint8_t>(b >> 16);
                    p += 3;
                }
            });
        }
    };

    const unsigned mt = static_cast<unsigned>(std::min<std::size_t>(threads, chunks));
    detail::run_threads(mt, [&](unsigned) { worker(); });

    return blocks;
}

[[nodiscard]] inline std::vector<std::vector<std::uint8_t>> encode_notes(
    const std::vector<Note>& notes, unsigned threads = 0) {
    static const std::vector<ChannelEvent> none;
    return encode_notes(notes, none, threads);
}

}  // namespace andromeda::midi::fast_export