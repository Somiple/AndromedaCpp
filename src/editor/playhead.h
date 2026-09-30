#pragma once

#include <memory>

#include "audio/audio_engine.h"
#include "editor/midi_types.h"
#include "util/shared.h"

namespace andromeda::editor {

class Playhead {
public:
    Playhead() = default;

    Playhead(MIDITick start_tick, util::SharedMutPtr<std::shared_ptr<audio::AudioEngine>> pm)
        : playback_manager_(std::move(pm)) {
        set_start(start_tick);
    }

    void set_start(MIDITick start_tick);

    MIDITick start_tick = 0;

private:
    util::SharedMutPtr<std::shared_ptr<audio::AudioEngine>> playback_manager_;
};

}
