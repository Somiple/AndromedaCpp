#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "editor/tempo_map.h"
#include "midi/events/meta_event.h"
#include "midi/midi_file.h"
#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::editor {

struct ProjectInfo {
    std::string name;
    std::string author;
    std::string description;
};

struct ProjectData {
    ProjectData()
        : global_metas(util::make_shared_rw<std::vector<MetaEvent>>()),
          tracks(util::make_shared_rw<std::vector<midi::MIDITrack>>()),
          tempo_map(util::make_shared_rw<TempoMap>()) {}

    void load_data_from_midi_file(midi::MIDIFile& midi_file);

    void reset_or_init_data();

    void validate_tracks(std::uint16_t track);

    std::uint16_t ppq = 0;
    SharedMetaEvents global_metas;
    util::SharedPtr<std::vector<midi::MIDITrack>> tracks;
    util::SharedPtr<TempoMap> tempo_map;
};

}
