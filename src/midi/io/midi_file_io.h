#pragma once

// ---------------------------------------------------------------------------
// Layer: midi
// Dependency direction:  core -> midi -> editor -> audio -> ui
// Headless MIDI file i/o: no ImGui, no dialogs, no editor/UI types. This is the
// layer a tool or a test can use without linking the whole UI.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "midi/events/meta_event.h"
#include "midi/midi_track.h"

namespace andromeda::midi {

/**
 * @brief Outcome of a MIDI file operation.
 *
 * Failures are reported here instead of being thrown, matching midi::export_midi_file.
 */
struct MidiIoResult {
    bool ok = false;        ///< true when the file was written completely
    std::string message;    ///< human readable error text; empty when ok
};

/**
 * @brief Headless MIDI file writer.
 *
 * Wraps midi::export_midi_file so callers do not have to know the export options or the
 * expected/error type. Deliberately contains no prompting and no project knowledge.
 *
 * @note Import is *not* duplicated here: it already lives in
 *       editor::ProjectManager::import_from_midi_file, which owns parsing and the
 *       project swap. Adding a second parse path would duplicate that policy.
 */
class MidiFileIo {
public:
    /**
     * @brief Writes @p tracks and @p global_metas as a format 1 MIDI file at @p path.
     * @param path destination .mid file; parent directories must exist.
     * @param ppq pulses per quarter note to encode in the header.
     * @param global_metas conductor-track meta events.
     * @param tracks one MIDI track per entry, in project order.
     * @return ok/message; never throws.
     */
    [[nodiscard]] static MidiIoResult export_to_path(const std::filesystem::path& path,
                                                     std::uint16_t ppq,
                                                     const std::vector<MetaEvent>& global_metas,
                                                     const std::vector<MIDITrack>& tracks);
};

} // namespace andromeda::midi
