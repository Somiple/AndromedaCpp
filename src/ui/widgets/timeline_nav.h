#pragma once

#include <algorithm>
#include <optional>

#include "ui/app/editor_app.h"
#include "editor/navigation.h"

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

inline void zoom_lane_around_mouse(const EditorApp& app, editor::PianoRollNavigation& nav, float fac) {
    const float mouse_pos_px_y = app.main_window()->get_mouse_pos()->y;
    const auto rect = app.main_window()->active_view().context.work_rect;

    // invert anchor because y coordinates are inversed
    const float anchor = 1.0f - std::clamp((mouse_pos_px_y - rect.top) / rect.height, 0.0f, 1.0f);

    const float old_zoom = nav.zoom_keys;
    const float old_pos = nav.key_pos;
    const float key_under_mouse = old_pos + anchor * old_zoom;
    nav.zoom_keys_by(fac);

    nav.key_pos = std::clamp(key_under_mouse - anchor * nav.zoom_keys, 0.0f, 128.0f - nav.zoom_keys);
}

inline void zoom_lane_around_mouse(const EditorApp& app, editor::TrackViewNavigation& nav, float fac) {
    const float mouse_pos_px_y = app.main_window()->get_mouse_pos()->y;
    const auto rect = app.main_window()->active_view().context.work_rect;
    const float anchor = std::clamp((mouse_pos_px_y - rect.top) / rect.height, 0.0f, 1.0f);

    const float old_zoom = nav.zoom_tracks;
    const float old_pos = nav.track_pos;
    const float track_under_mouse = old_pos + anchor * old_zoom;
    nav.zoom_tracks_by(fac);

    nav.track_pos = std::max(track_under_mouse - anchor * nav.zoom_tracks, 0.0f);
}

}
