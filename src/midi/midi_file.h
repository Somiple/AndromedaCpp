#pragma once

#include <cstdint>
#include <expected>
#include <iosfwd>
#include <string>
#include <string_view>
#include <vector>
#include <filesystem>
#include <thread>

#include "editor/midi_types.h"
#include "midi/events/channel_event.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"
#include "midi/midi_track.h"
#include "midi/midi_track_parser.h"

namespace andromeda::midi {

using andromeda::editor::MIDITick;

struct MIDITrackPointer {
    std::uint64_t start;
    std::uint32_t length;
};

class MIDIFile {
public:
    MIDIFile() = default;

    MIDIFile& with_track_discarding(bool value);

    std::expected<void, std::string> open(std::string_view path);

    void preprocess_meta_events();

    std::uint16_t format = 0;
    std::uint16_t trk_count = 0;
    std::uint16_t ppq = 0;

    std::vector<MetaEvent> global_meta_events;

    std::vector<MIDITrack> tracks;

private:
    [[nodiscard]] static std::vector<std::uint32_t> merge_tree_leaf_rank(std::size_t count);

    [[nodiscard]] std::vector<MetaEvent> merge_meta_events(
        std::vector<std::vector<MetaEvent>> seq) const;

    static std::uint16_t bytes_to_u16(const std::uint8_t* bytes);
    static std::uint32_t bytes_to_u32(const std::uint8_t* bytes);

    bool track_discarding_ = false;

    std::size_t per_track_metas_ = 0;
    std::size_t global_metas_ = 0;
};

struct MIDIEvent {
    MIDITick delta = 0;
    std::vector<std::uint8_t> data;

    std::expected<void, std::string> write_to(std::ostream& w) const;
    std::expected<void, std::string> write_delta_to(std::ostream& w) const;

    [[nodiscard]] std::size_t vlq_len() const;
};

class MIDIFileWriter {
public:
    explicit MIDIFileWriter(std::uint16_t ppq) : ppq_(ppq) {}

    std::size_t new_track();

    std::size_t append_track(std::vector<MIDIEvent> track);

    // A track is an ordered list of pieces. Legacy callers contribute event pieces; the bulk
    // note path contributes raw pieces that are already serialized (delta VLQ + 3-byte event).
    struct Piece {
        std::vector<MIDIEvent> events;
        std::vector<std::uint8_t> raw;
        bool is_raw = false;
    };
    using EncodedTrack = std::vector<Piece>;

    // Serializes one complete track body (notes + channel events + end-of-track) without
    // needing a writer instance, so tracks can be encoded concurrently and handed over with
    // append_encoded_track(). threads: 0 = use all cores, 1 = stay on the calling thread.
    [[nodiscard]] static EncodedTrack encode_track(const std::vector<Note>& notes,
        const std::vector<ChannelEvent>& channel_events,
        unsigned threads = 0, std::string_view track_name = {});

    std::size_t append_encoded_track(EncodedTrack track);

    std::vector<MIDIEvent> into_single_track() &&;

    void flush_evs_to_track(std::vector<MIDIEvent> events);

    void end_track();

    void flush_global_metas(const std::vector<MetaEvent>& meta_events);

    void add_notes_to_midi(const std::vector<Note>& notes, unsigned threads = 0);

    void add_notes_with_other_events(const std::vector<Note>& notes,
                                     const std::vector<ChannelEvent>& events, unsigned threads = 0);

    std::expected<void, std::string> write_midi(const std::filesystem::path& path) const;

private:
    [[nodiscard]] std::vector<MIDIEvent> notes_to_events(std::vector<const Note*> notes) const;

    static std::expected<void, std::string> write_u32(std::ostream& writer, std::uint32_t val);
    static std::expected<void, std::string> write_u16(std::ostream& writer, std::uint16_t val);

    std::uint16_t ppq_;
    std::uint16_t track_count_ = 0;
    std::vector<EncodedTrack> tracks_;};

}