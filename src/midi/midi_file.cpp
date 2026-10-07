#include "midi/midi_file.h"

#include <algorithm>
#include <deque>
#include <atomic>
#include <cassert>
#include <format>
#include <fstream>
#include <numeric>
#include <queue>
#include <thread>
#include <utility>

#include "midi/events/mergers.h"
#include "midi/io/mapped_file.h"
#include "util/debugger.h"

namespace andromeda::midi {

MIDIFile& MIDIFile::with_track_discarding(bool value) {
    track_discarding_ = value;
    return *this;
}

std::expected<void, std::string> MIDIFile::open(std::string_view path) {
    auto mapped = MappedFile::open(path);
    if (!mapped) {
        return std::unexpected(mapped.error());
    }

    const MappedFile& file = *mapped;
    const std::uint8_t* const base = file.data();
    const std::size_t size = file.size();

    if (size < 14 || bytes_to_u32(base) != 0x4D546864 || bytes_to_u32(base + 4) != 6) {
        return std::unexpected("Invalid file header!");
    }

    const std::uint16_t hdr_format = bytes_to_u16(base + 8);
    const std::uint16_t hdr_trk_count = bytes_to_u16(base + 10);
    const std::uint16_t hdr_ppq = bytes_to_u16(base + 12);

    std::jthread prefetcher([&file] { file.prefetch(); });

    std::vector<MIDITrackPointer> track_locations;
    track_locations.reserve(hdr_trk_count);

    std::size_t pos = 14;
    for (std::uint16_t i = 0; i < hdr_trk_count; ++i) {
        if (pos + 8 > size || bytes_to_u32(base + pos) != 0x4D54726B) {
            return std::unexpected("Invalid track header!");
        }

        std::uint64_t length = bytes_to_u32(base + pos + 4);
        const std::size_t start = pos + 8;

        if (length > size - start) {
            length = size - start;
        }

        track_locations.push_back(
            MIDITrackPointer{.start = start, .length = static_cast<std::uint32_t>(length)});
        pos = start + static_cast<std::size_t>(length);
    }

    const std::size_t track_count = track_locations.size();
    tracks.clear();
    tracks.resize(track_count);

    std::vector<std::uint32_t> order(track_count);
    std::iota(order.begin(), order.end(), std::uint32_t{0});
    std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
        return track_locations[a].length > track_locations[b].length;
    });

    std::atomic<std::size_t> next_job{0};

    const auto worker = [&] {
        TrackParseScratch scratch;

        for (;;) {
            const std::size_t job = next_job.fetch_add(1, std::memory_order_relaxed);
            if (job >= track_count) {
                break;
            }

            const std::size_t index = order[job];
            const MIDITrackPointer& location = track_locations[index];

            MIDITrackParser parser(base + location.start, location.length);
            parser.parse_all(scratch);

            MIDITrack& track = tracks[index];
            track.notes = std::move(parser.note_events);
            track.channel_events = std::move(parser.channel_events);
            track.meta_events = std::move(parser.meta_events);
        }
    };

    unsigned int hw = std::thread::hardware_concurrency();
    if (hw == 0) {
        hw = 4;
    }
    const std::size_t thread_count = std::min<std::size_t>(hw, std::max<std::size_t>(track_count, 1));

    if (thread_count <= 1) {
        worker();
    } else {
        std::vector<std::jthread> workers;
        workers.reserve(thread_count - 1);
        for (std::size_t i = 1; i < thread_count; ++i) {
            workers.emplace_back(worker);
        }
        worker();
    }

    this->format = hdr_format;
    this->trk_count = hdr_trk_count;
    this->ppq = hdr_ppq;

    return {};
}

void MIDIFile::preprocess_meta_events() {
    std::vector<std::vector<MetaEvent>> mergeable;
    mergeable.reserve(tracks.size());

    for (MIDITrack& track : tracks) {
        std::vector<MetaEvent> m_track;

        std::vector<MetaEvent>& meta_evs_ = track.get_meta_events_mut();
        std::vector<MetaEvent> meta_evs = std::move(meta_evs_);
        meta_evs_.clear();

        for (MetaEvent& meta_ev : meta_evs) {
            switch (meta_ev.event_type) {
            case MetaEventType::Tempo:
            case MetaEventType::TimeSignature:
            case MetaEventType::KeySignature:
            case MetaEventType::Lyric:
            case MetaEventType::Marker:
                m_track.push_back(std::move(meta_ev));
                break;
            // saves a tiny bit of memory!
            case MetaEventType::TrackName: {
                if (meta_ev.data.size() > 0) {
                    std::string track_name(meta_ev.data.begin(), meta_ev.data.end());
                    track.name = track_name;
                }
                break;
            }
            default:
                meta_evs_.push_back(std::move(meta_ev));
                break;
            }
        }

        mergeable.push_back(std::move(m_track));
    }

    global_meta_events = merge_meta_events(std::move(mergeable));
}

std::vector<std::uint32_t> MIDIFile::merge_tree_leaf_rank(std::size_t count) {
    std::vector<std::uint32_t> rank(count, 0);
    if (count == 0) {
        return rank;
    }

    std::deque<std::vector<std::uint32_t>> queue;
    for (std::uint32_t i = 0; i < count; ++i) {
        queue.push_back({i});
    }

    while (queue.size() > 1) {
        std::vector<std::uint32_t> left = std::move(queue.front());
        queue.pop_front();
        const std::vector<std::uint32_t> right = std::move(queue.front());
        queue.pop_front();

        left.insert(left.end(), right.begin(), right.end());
        queue.push_back(std::move(left));
    }

    const std::vector<std::uint32_t>& order = queue.front();
    for (std::uint32_t at = 0; at < order.size(); ++at) {
        rank[order[at]] = at;
    }

    return rank;
}

std::vector<MetaEvent> MIDIFile::merge_meta_events(std::vector<std::vector<MetaEvent>> seq) const {
    // ties must follow rust merge tree leaf rank (empties included), not track index
    std::vector<std::uint32_t> rank = merge_tree_leaf_rank(seq.size());

    std::size_t total = 0;
    std::vector<std::vector<MetaEvent>*> live;
    std::vector<std::uint32_t> live_rank;
    live.reserve(seq.size());
    live_rank.reserve(seq.size());

    for (std::uint32_t i = 0; i < seq.size(); ++i) {
        if (!seq[i].empty()) {
            live.push_back(&seq[i]);
            live_rank.push_back(rank[i]);
            total += seq[i].size();
        }
    }

    if (live.empty()) {
        return {};
    }
    if (live.size() == 1) {
        return std::move(*live.front());
    }

    std::vector<MetaEvent> out;
    out.reserve(total);

    struct Head {
        MIDITick tick;
        std::uint32_t seq;
        std::uint32_t rank;
    };

    const auto later = [](const Head& a, const Head& b) {
        return a.tick != b.tick ? a.tick > b.tick : a.rank > b.rank;
    };

    std::vector<std::size_t> pos(live.size(), 0);
    std::vector<Head> heap;
    heap.reserve(live.size());

    for (std::uint32_t i = 0; i < live.size(); ++i) {
        heap.push_back(Head{(*live[i])[0].tick, i, live_rank[i]});
    }
    std::make_heap(heap.begin(), heap.end(), later);

    while (!heap.empty()) {
        std::pop_heap(heap.begin(), heap.end(), later);
        const Head head = heap.back();
        heap.pop_back();

        std::vector<MetaEvent>& src = *live[head.seq];
        std::size_t at = pos[head.seq];

        if (heap.empty()) {
            for (; at < src.size(); ++at) {
                out.push_back(std::move(src[at]));
            }
        } else {
            const Head next = heap.front();
            do {
                out.push_back(std::move(src[at]));
                ++at;
            } while (at < src.size() && (src[at].tick < next.tick ||
                                         (src[at].tick == next.tick && head.rank < next.rank)));
        }

        pos[head.seq] = at;
        if (at < src.size()) {
            heap.push_back(Head{src[at].tick, head.seq, head.rank});
            std::push_heap(heap.begin(), heap.end(), later);
        }
    }

    return out;
}

std::uint16_t MIDIFile::bytes_to_u16(const std::uint8_t* bytes) {
    return static_cast<std::uint16_t>((static_cast<std::uint16_t>(bytes[0]) << 8) |
                                      static_cast<std::uint16_t>(bytes[1]));
}

std::uint32_t MIDIFile::bytes_to_u32(const std::uint8_t* bytes) {
    return (static_cast<std::uint32_t>(bytes[0]) << 24) |
           (static_cast<std::uint32_t>(bytes[1]) << 16) |
           (static_cast<std::uint32_t>(bytes[2]) << 8) |
           (static_cast<std::uint32_t>(bytes[3]) << 0);
}

std::expected<void, std::string> MIDIEvent::write_to(std::ostream& w) const {
    if (auto r = write_delta_to(w); !r) {
        return r;
    }
    w.write(reinterpret_cast<const char*>(data.data()),
            static_cast<std::streamsize>(data.size()));
    if (!w) {
        return std::unexpected("write failed");
    }
    return {};
}

std::expected<void, std::string> MIDIEvent::write_delta_to(std::ostream& w) const {
    MIDITick d = delta;

    std::uint8_t buf[5] = {};
    std::size_t i = 0;
    buf[0] = static_cast<std::uint8_t>(d & 0x7F);
    d >>= 7;

    while (d > 0) {
        i += 1;

        buf[i] = static_cast<std::uint8_t>((d & 0x7F) | 0x80);
        d >>= 7;
    }

    for (std::size_t idx = i + 1; idx-- > 0;) {
        w.write(reinterpret_cast<const char*>(&buf[idx]), 1);
    }

    if (!w) {
        return std::unexpected("write failed");
    }
    return {};
}

std::size_t MIDIEvent::vlq_len() const {
    MIDITick d = delta;
    if (d == 0) {
        return 1;
    }

    std::size_t len = 0;
    while (d > 0) {
        len += 1;
        d >>= 7;
    }

    return len;
}

namespace {

struct TimedEvent {
    MIDITick time;
    MIDIEvent event;
};

struct TimedEventCompare {
    bool operator()(const TimedEvent& a, const TimedEvent& b) const { return a.time > b.time; }
};

}

std::size_t MIDIFileWriter::new_track() {
    tracks_.emplace_back();
    track_count_ += 1;
    return static_cast<std::size_t>(track_count_) - 1;
}

std::size_t MIDIFileWriter::append_track(std::vector<MIDIEvent> track) {
    tracks_.push_back(std::move(track));
    track_count_ += 1;
    return static_cast<std::size_t>(track_count_) - 1;
}

std::vector<MIDIEvent> MIDIFileWriter::into_single_track() && {
    assert(tracks_.size() == 1 && "Writer must contain exactly 1 track.");
    return std::move(tracks_.front());
}

void MIDIFileWriter::flush_evs_to_track(std::vector<MIDIEvent> events) {
    auto& track = tracks_[static_cast<std::size_t>(track_count_) - 1];
    track.insert(track.end(), std::make_move_iterator(events.begin()),
                 std::make_move_iterator(events.end()));
}

void MIDIFileWriter::end_track() {
    tracks_[static_cast<std::size_t>(track_count_) - 1].push_back(
        MIDIEvent{.delta = 0, .data = {0xFF, 0x2F, 0x00}});
}

void MIDIFileWriter::flush_global_metas(const std::vector<MetaEvent>& meta_events) {
    new_track();

    std::vector<MIDIEvent> seq;
    MIDITick prev_time = 0;

    for (const MetaEvent& meta_event : meta_events) {
        std::vector<std::uint8_t> data = {
            0xFF,
            static_cast<std::uint8_t>(meta_event.event_type),
            static_cast<std::uint8_t>(meta_event.data.size())};
        data.insert(data.end(), meta_event.data.begin(), meta_event.data.end());

        seq.push_back(MIDIEvent{.delta = meta_event.tick - prev_time, .data = std::move(data)});
        prev_time = meta_event.tick;
    }

    flush_evs_to_track(std::move(seq));
    end_track();
}

void MIDIFileWriter::add_notes_to_midi(const std::vector<Note>& notes) {
    if (notes.empty()) {
        return;
    }

    std::vector<const Note*> sorted;
    sorted.reserve(notes.size());
    for (const Note& n : notes) {
        sorted.push_back(&n);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Note* a, const Note* b) { return a->start < b->start; });

    flush_evs_to_track(notes_to_events(std::move(sorted)));
}

void MIDIFileWriter::add_notes_with_other_events(const std::vector<Note>& notes,
                                                 const std::vector<ChannelEvent>& events) {
    if (notes.empty()) {
        return;
    }

    if (events.empty()) {
        add_notes_to_midi(notes);
        return;
    }

    std::vector<const Note*> sorted;
    sorted.reserve(notes.size());
    for (const Note& n : notes) {
        sorted.push_back(&n);
    }
    std::stable_sort(sorted.begin(), sorted.end(),
                     [](const Note* a, const Note* b) { return a->start < b->start; });

    std::vector<MIDIEvent> notes_conv = notes_to_events(std::move(sorted));
    std::vector<MIDIEvent> chans_cov = channel_to_midi_ev(events);
    std::vector<MIDIEvent> merged = merge_events(std::move(notes_conv), std::move(chans_cov));
    flush_evs_to_track(std::move(merged));
}

std::vector<MIDIEvent> MIDIFileWriter::notes_to_events(std::vector<const Note*> notes) const {
    std::vector<MIDIEvent> seq;
    std::priority_queue<TimedEvent, std::vector<TimedEvent>, TimedEventCompare> note_offs;
    MIDITick prev_time = 0;

    for (const Note* note : notes) {
        while (!note_offs.empty()) {
            if (note_offs.top().time > note->start) {
                break;
            }

            TimedEvent te = std::move(const_cast<TimedEvent&>(note_offs.top()));
            note_offs.pop();

            te.event.delta = te.time - prev_time;
            prev_time = te.time;
            seq.push_back(std::move(te.event));
        }

        seq.push_back(MIDIEvent{
            .delta = note->get_start() - prev_time,
            .data = {static_cast<std::uint8_t>(0x90 | note->get_channel()),
                     note->get_key(),
                     note->get_velocity()}});

        prev_time = note->get_start();

        note_offs.push(TimedEvent{
            .time = note->get_start() + note->get_length(),
            .event = MIDIEvent{.delta = 0,
                               .data = {static_cast<std::uint8_t>(0x80 | note->get_channel()),
                                        note->key,
                                        0x00}}});
    }

    while (!note_offs.empty()) {
        TimedEvent te = std::move(const_cast<TimedEvent&>(note_offs.top()));
        note_offs.pop();

        te.event.delta = te.time - prev_time;
        prev_time = te.time;
        seq.push_back(std::move(te.event));
    }

    return seq;
}

std::expected<void, std::string> MIDIFileWriter::write_midi(std::string_view path) const {
    std::ofstream file(std::string(path), std::ios::binary);
    if (!file.is_open()) {
        return std::unexpected(std::format("could not create {}", path));
    }

    std::vector<char> iobuf(16 * 1024 * 1024);
    file.rdbuf()->pubsetbuf(iobuf.data(), static_cast<std::streamsize>(iobuf.size()));

    if (auto r = write_u32(file, 0x4D546864); !r) return r;

    if (auto r = write_u32(file, 6); !r) return r;
    if (auto r = write_u16(file, 1); !r) return r;
    if (auto r = write_u16(file, track_count_); !r) return r;
    if (auto r = write_u16(file, ppq_); !r) return r;

    for (const auto& track : tracks_) {
        std::uint64_t track_len = 0;

        for (const MIDIEvent& ev : track) {
            track_len += static_cast<std::uint64_t>(ev.vlq_len() + ev.data.size());
            if (track_len > 0xFFFFFFFFULL) {
                return std::unexpected("track length overflow");
            }
        }

        if (auto r = write_u32(file, 0x4D54726B); !r) return r;
        if (auto r = write_u32(file, static_cast<std::uint32_t>(track_len)); !r) return r;

        for (const MIDIEvent& ev : track) {
            if (auto r = ev.write_to(file); !r) return r;
        }
    }

    file.flush();
    if (!file) {
        return std::unexpected("write failed");
    }
    return {};
}

std::expected<void, std::string> MIDIFileWriter::write_u32(std::ostream& writer, std::uint32_t val) {
    const std::uint8_t b[4] = {static_cast<std::uint8_t>((val & 0xFF000000) >> 24),
                               static_cast<std::uint8_t>((val & 0xFF0000) >> 16),
                               static_cast<std::uint8_t>((val & 0xFF00) >> 8),
                               static_cast<std::uint8_t>(val & 0xFF)};
    writer.write(reinterpret_cast<const char*>(b), 4);
    if (!writer) {
        return std::unexpected("write failed");
    }
    return {};
}

std::expected<void, std::string> MIDIFileWriter::write_u16(std::ostream& writer, std::uint16_t val) {
    const std::uint8_t b[2] = {static_cast<std::uint8_t>((val & 0xFF00) >> 8),
                               static_cast<std::uint8_t>(val & 0xFF)};
    writer.write(reinterpret_cast<const char*>(b), 2);
    if (!writer) {
        return std::unexpected("write failed");
    }
    return {};
}

}
