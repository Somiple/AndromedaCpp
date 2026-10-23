#pragma once

#include <algorithm>
#include <optional>

#include "app/editor_app.h"

namespace andromeda::app {

// fixed rust bug: x zoom kept the left edge, so the seek point slid away from where it was
template <typename Nav>
void zoom_ticks_around_anchor(const EditorApp& app, Nav& n, float fac) {
    // TODO: zoom_anchor_tick belongs to app
    const std::optional<float> anchor =
        app.main_window()->zoom_anchor_tick(n.tick_pos_smoothed, n.zoom_ticks_smoothed);
    const float before = n.zoom_ticks;
    n.zoom_ticks_by(fac);
    if (anchor && before > 0.0f) {
        const float pos = *anchor + (n.tick_pos - *anchor) * (n.zoom_ticks / before);
        // while playing this is only the playhead's offset, so it may go below 0
        n.tick_pos = app.get_audio_engine()->get_current_engine()->is_playing() ? pos : std::max(0.0f, pos);
    }
}

}
