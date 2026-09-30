#pragma once

namespace andromeda::midi {

enum class MIDIParseStatus {
    ParseOK,
    ParseNotMIDI,
    ParseCorrupt,
    ParseError
};

}
