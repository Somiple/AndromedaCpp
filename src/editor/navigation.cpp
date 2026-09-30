#include "editor/navigation.h"

#include <cmath>

namespace andromeda::editor {

void PianoRollNavigation::update_smoothed_values(float dt) {
    const float smooth = 15.0f;
    const float alpha = 1.0f - std::exp(-smooth * dt);

    tick_pos_smoothed += (tick_pos - tick_pos_smoothed) * alpha;
    key_pos_smoothed += (key_pos - key_pos_smoothed) * alpha;
    zoom_ticks_smoothed += (zoom_ticks - zoom_ticks_smoothed) * alpha;
    zoom_keys_smoothed += (zoom_keys - zoom_keys_smoothed) * alpha;
}

bool PianoRollNavigation::smoothed_values_needs_update() const {
    const float tick_diff = std::abs(tick_pos - tick_pos_smoothed);
    const float key_diff = std::abs(key_pos - key_pos_smoothed);
    const float zoom_tick_diff = std::abs(zoom_ticks - zoom_ticks_smoothed);
    const float zoom_key_diff = std::abs(zoom_keys - zoom_keys_smoothed);
    return tick_diff > 0.001f || key_diff > 0.001f || zoom_tick_diff > 0.001f ||
           zoom_key_diff > 0.001f;
}

void PianoRollNavigation::zoom_ticks_by(float fac) {
    float new_zoom_ticks = zoom_ticks * fac;
    if (new_zoom_ticks < static_cast<float>(PR_ZOOM_TICKS_MIN)) {
        new_zoom_ticks = static_cast<float>(PR_ZOOM_TICKS_MIN);
    }
    if (new_zoom_ticks > static_cast<float>(PR_ZOOM_TICKS_MAX)) {
        new_zoom_ticks = static_cast<float>(PR_ZOOM_TICKS_MAX);
    }
    zoom_ticks = new_zoom_ticks;
}

void PianoRollNavigation::zoom_keys_by(float fac) {
    float new_zoom_keys = zoom_keys * fac;
    if (new_zoom_keys < 12.0f) {
        new_zoom_keys = 12.0f;
    }
    if (new_zoom_keys > 128.0f) {
        new_zoom_keys = 128.0f;
    }

    const float view_top = key_pos + zoom_keys;
    zoom_keys = new_zoom_keys;
    const float view_top_new = key_pos + zoom_keys;
    const float view_top_delta = view_top_new - view_top;

    if (view_top_new > 128.0f) {
        key_pos -= view_top_delta;
    }
    if (key_pos < 0.0f) {
        key_pos = 0.0f;
    }
}

void TrackViewNavigation::update_smoothed_values(float dt) {
    const float smooth = 15.0f;
    const float alpha = 1.0f - std::exp(-smooth * dt);

    tick_pos_smoothed += (tick_pos - tick_pos_smoothed) * alpha;
    track_pos_smoothed += (track_pos - track_pos_smoothed) * alpha;
    zoom_ticks_smoothed += (zoom_ticks - zoom_ticks_smoothed) * alpha;
    zoom_tracks_smoothed += (zoom_tracks - zoom_tracks_smoothed) * alpha;
}

bool TrackViewNavigation::smoothed_values_needs_update() const {
    const float tick_diff = std::abs(tick_pos - tick_pos_smoothed);
    const float track_diff = std::abs(track_pos - track_pos_smoothed);
    const float zoom_tick_diff = std::abs(zoom_ticks - zoom_ticks_smoothed);
    const float zoom_track_diff = std::abs(zoom_tracks - zoom_tracks_smoothed);
    return tick_diff > 0.001f || track_diff > 0.001f || zoom_tick_diff > 0.001f ||
           zoom_track_diff > 0.001f;
}

void TrackViewNavigation::zoom_ticks_by(float fac) {
    float new_zoom_ticks = zoom_ticks * fac;
    if (new_zoom_ticks < static_cast<float>(TV_ZOOM_TICKS_MIN)) {
        new_zoom_ticks = static_cast<float>(TV_ZOOM_TICKS_MIN);
    }
    if (new_zoom_ticks > static_cast<float>(TV_ZOOM_TICKS_MAX)) {
        new_zoom_ticks = static_cast<float>(TV_ZOOM_TICKS_MAX);
    }
    zoom_ticks = new_zoom_ticks;
}

void TrackViewNavigation::zoom_tracks_by(float fac) {
    float new_zoom_tracks = zoom_tracks * fac;
    if (new_zoom_tracks < 10.0f) {
        new_zoom_tracks = 10.0f;
    }
    if (new_zoom_tracks > 64.0f) {
        new_zoom_tracks = 64.0f;
    }
    zoom_tracks = new_zoom_tracks;
}

}
