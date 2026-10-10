#pragma once

#include <algorithm>
#include <optional>

#include "app/main_window.h"

namespace andromeda::app {

// fixed rust bug: x zoom kept the left edge, so the seek point slid away from where it was
template <typename Nav>
void zoom_ticks_around_anchor(const MainWindow& parent, Nav& n, float fac) {
    const std::optional<float> anchor =
        parent.zoom_anchor_tick(n.tick_pos_smoothed, n.zoom_ticks_smoothed);
    const float before = n.zoom_ticks;
    n.zoom_ticks_by(fac);
    if (anchor && before > 0.0f) {
        const float pos = *anchor + (n.tick_pos - *anchor) * (n.zoom_ticks / before);
        // while playing this is only the playhead's offset, so it may go below 0
        n.tick_pos = parent.audio_engine->is_playing() ? pos : std::max(0.0f, pos);
    }
}

}
