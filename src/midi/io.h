#pragma once

// ---------------------------------------------------------------------------
// Layer: midi
// Dependency direction:  core -> midi -> editor -> audio -> ui
// This layer may include `core/...` only. Keep it free of ImGui, GLFW, GL and
// of editor/ui types so MIDI I/O stays reusable by tools.
// See docs/ARCHITECTURE.md.
// ---------------------------------------------------------------------------

namespace andromeda::midi {

enum class MIDIParseStatus {
    ParseOK,
    ParseNotMIDI,
    ParseCorrupt,
    ParseError
};

}
