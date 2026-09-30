#pragma once

#include <cstdint>
#include <variant>

#include "editor/midi_types.h"

namespace andromeda::midi {

using andromeda::editor::MIDITick;

struct NoteOff {
    std::uint8_t key;
    bool operator==(const NoteOff&) const = default;
};
struct NoteOn {
    std::uint8_t key;
    std::uint8_t velocity;
    bool operator==(const NoteOn&) const = default;
};
struct NoteAftertouch {
    std::uint8_t key;
    std::uint8_t pressure;
    bool operator==(const NoteAftertouch&) const = default;
};
struct Controller {
    std::uint8_t controller;
    std::uint8_t value;
    bool operator==(const Controller&) const = default;
};
struct ProgramChange {
    std::uint8_t program;
    bool operator==(const ProgramChange&) const = default;
};
struct ChannelAftertouch {
    std::uint8_t amount;
    bool operator==(const ChannelAftertouch&) const = default;
};
struct PitchBend {
    std::uint8_t lsb;
    std::uint8_t msb;
    bool operator==(const PitchBend&) const = default;
};

using ChannelEventType = std::variant<
    NoteOff,
    NoteOn,
    NoteAftertouch,
    Controller,
    ProgramChange,
    ChannelAftertouch,
    PitchBend>;

struct ChannelEvent {
    MIDITick tick = 0;
    std::uint8_t channel = 0;
    ChannelEventType event_type{};

    bool operator==(const ChannelEvent&) const = default;
};

}
