#pragma once

#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <vector>

#include "midi/events/meta_event.h"
#include "midi/midi_track.h"

namespace andromeda::midi {

struct MIDIExportOptions {
    // 0: every hardware thread
    unsigned threads = 0;
    // 0: tuned default. otherwise about this many events per unit of parallel work; tests
    // pass tiny values to reach every boundary on small input. the bytes never depend on it
    std::size_t grain = 0;
};

// writes a format 1 file: a conductor track with global_metas, then one track per entry of
// tracks. the input must not change during the call. never throws
[[nodiscard]] std::expected<void, std::string> export_midi_file(
    const std::filesystem::path& path, std::uint16_t ppq,
    const std::vector<MetaEvent>& global_metas, const std::vector<MIDITrack>& tracks,
    const MIDIExportOptions& options = {});

}
