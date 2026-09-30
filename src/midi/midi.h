#pragma once

#include <cstdint>

namespace andromeda::midi {

enum class MIDIParseMode {
    Full,
    AnalyzeOnly
};

inline constexpr std::uint8_t MIDI_KEY_MIN = 0;
inline constexpr std::int8_t MIDI_KEY_MIN_SIGNED = 0;
inline constexpr std::uint8_t MIDI_KEY_MAX = 127;
inline constexpr std::int8_t MIDI_KEY_MAX_SIGNED = 127;

}
