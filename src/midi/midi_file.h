#pragma once

#include <cstdint>
#include <expected>
#include <string>
#include <string_view>
#include <vector>

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

}
