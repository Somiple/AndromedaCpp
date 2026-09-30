#pragma once

#include <cstdint>

#include "editor/midi_types.h"

namespace andromeda::midi {

using andromeda::editor::MIDITick;

struct Note {
    MIDITick start = 0;
    MIDITick length = 0;
    std::uint8_t key = 0;
    std::uint8_t velocity = 0;
    std::uint8_t channel = 0;

    [[nodiscard]] MIDITick get_start() const { return start; }
    [[nodiscard]] MIDITick get_length() const { return length; }
    [[nodiscard]] std::uint8_t get_key() const { return key; }
    [[nodiscard]] std::uint8_t get_velocity() const { return velocity; }
    [[nodiscard]] std::uint8_t get_channel() const { return channel; }

    [[nodiscard]] MIDITick end() const { return get_start() + get_length(); }

    void set_start(MIDITick v) { start = v; }
    void set_length(MIDITick v) { length = v; }
    void set_key(std::uint8_t v) { key = v; }
    void set_velocity(std::uint8_t v) { velocity = v; }
    void set_end(MIDITick e) { set_length(e - get_start()); }

    bool operator==(const Note&) const = default;
};

}
