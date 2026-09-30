#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

namespace andromeda::audio {

class MIDIAudioEngine {
public:
    virtual ~MIDIAudioEngine() = default;

    virtual void init_audio() = 0;
    virtual void close_stream() = 0;

    virtual std::expected<void, std::string> send_event(std::span<const std::uint8_t> raw_event) = 0;

    virtual void send_events(std::span<const std::array<std::uint8_t, 3>> events) {
        for (const std::array<std::uint8_t, 3>& ev : events) {
            (void)send_event(ev);
        }
    }
};

}
