#include "editor/playhead.h"

namespace andromeda::editor {

void Playhead::set_start(MIDITick new_start_tick) {
    start_tick = new_start_tick;

    if (playback_manager_) {
        std::lock_guard lock(playback_manager_->mutex);
        playback_manager_->value->navigate_to(new_start_tick);
    }
}

}
