#include "editor/playhead.h"
#include "app/main_window.h"
#include "audio/audio_timer.h"

namespace andromeda::editor {

void Playhead::set_start(MIDITick new_start_tick) {
    start_tick = new_start_tick;

    if (!app_) return;

    const auto* timeline = app_->editor_controller.get_project_manager()->get_timeline();
    audio::AudioTimer* timer = app_->get_timer();
    timer->navigate_to(timeline->tick_to_secs(new_start_tick));
}

}
