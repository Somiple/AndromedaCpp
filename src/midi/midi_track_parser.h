#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

#include "editor/midi_types.h"
#include "midi/events/channel_event.h"
#include "midi/events/meta_event.h"
#include "midi/events/note.h"

namespace andromeda::midi {

using andromeda::editor::MIDITick;

class TrackParseScratch {
public:
    TrackParseScratch();

    void reset();

    struct Pending {
        std::uint32_t head = 0;
        std::vector<std::uint32_t> ids;
    };

    std::vector<Pending> slots;
};

class MIDITrackParser {
public:
    MIDITrackParser(const std::uint8_t* data, std::size_t length);

    void parse_all(TrackParseScratch& scratch);

    std::vector<Note> note_events;
    std::vector<ChannelEvent> channel_events;
    std::vector<MetaEvent> meta_events;
    bool parse_success = true;
    bool track_ended = false;

private:
    template <bool Store>
    void run(TrackParseScratch* scratch);

    const std::uint8_t* begin_ = nullptr;
    const std::uint8_t* end_ = nullptr;

    std::uint64_t counted_notes_ = 0;
    std::uint64_t counted_channel_evs_ = 0;
};

}
