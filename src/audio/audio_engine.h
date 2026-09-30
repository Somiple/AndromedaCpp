#pragma once

#include <cstdint>
#include <memory>

#include "app/app_event_listener.h"
#include "audio/midi_audio_engine.h"
#include "editor/midi_types.h"
#include "util/shared.h"

namespace andromeda::audio {

using andromeda::editor::MIDITick;

class AudioEngine : public app::AppEventListener {
public:
    virtual void init() {}
    virtual void navigate_to(MIDITick tick) = 0;
    [[nodiscard]] virtual MIDITick get_playback_ticks() const = 0;
    [[nodiscard]] virtual MIDITick get_playback_start_tick() const = 0;
    virtual void start_playback() = 0;
    virtual void stop() = 0;
    virtual void toggle_playback() = 0;
    [[nodiscard]] virtual bool is_playing() const = 0;
    virtual void switch_device(util::SharedMutPtr<std::shared_ptr<MIDIAudioEngine>> device) {
        (void)device;
    }
    virtual void reset_events() = 0;

    virtual void start_play_at_mouse(std::uint8_t key, std::uint8_t channel,
                                     std::uint8_t velocity) {
        (void)key;
        (void)channel;
        (void)velocity;
    }
    virtual void update_play_at_mouse(std::uint8_t key, std::uint8_t channel,
                                      std::uint8_t velocity) {
        (void)key;
        (void)channel;
        (void)velocity;
    }
    virtual void stop_play_at_mouse(std::uint8_t key, std::uint8_t channel) {
        (void)key;
        (void)channel;
    }
};

using SharedDevice = util::SharedMutPtr<std::shared_ptr<MIDIAudioEngine>>;

}
