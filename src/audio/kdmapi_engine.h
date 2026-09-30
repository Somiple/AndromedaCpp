#pragma once

#include <array>
#include <cstdint>
#include <expected>
#include <span>
#include <string>

#include "audio/midi_audio_engine.h"

namespace andromeda::audio::kdmapi {

class KDMAPI : public MIDIAudioEngine {
public:
    KDMAPI();
    ~KDMAPI() override;

    KDMAPI(const KDMAPI&) = delete;
    KDMAPI& operator=(const KDMAPI&) = delete;

    void init();
    void close();

    void init_audio() override;
    void close_stream() override;
    std::expected<void, std::string> send_event(std::span<const std::uint8_t> raw_event) override;
    void send_events(std::span<const std::array<std::uint8_t, 3>> events) override;

private:
    struct Lib;

    static Lib* load_lib();

    bool stream_open_ = false;
};

}
