// refactor: new translation unit. the byte-level MIDI writing that used to sit inside
// ui/app/midi_io.cpp now lives in the midi layer, where it stays usable without ImGui,
// dialogs or editor types. behaviour is unchanged: same exporter, same options.

#include "midi/io/midi_file_io.h"

#include "midi/midi_export.h"

namespace andromeda::midi {

MidiIoResult MidiFileIo::export_to_path(const std::filesystem::path& path,
                                        std::uint16_t ppq,
                                        const std::vector<MetaEvent>& global_metas,
                                        const std::vector<MIDITrack>& tracks) {
    // delegate to the existing exporter: this class must not re-implement encoding.
    const auto written = midi::export_midi_file(path, ppq, global_metas, tracks);

    if (!written) {
        return MidiIoResult{.ok = false, .message = written.error()};
    }

    return MidiIoResult{.ok = true, .message = {}};
}

} // namespace andromeda::midi
