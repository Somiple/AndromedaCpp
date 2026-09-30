#pragma once

#include "editor/util.h"

namespace andromeda::editor {

inline constexpr MIDITick PR_ZOOM_TICKS_MIN = 48;
inline constexpr MIDITick PR_ZOOM_TICKS_MAX = 384000;
inline constexpr MIDITick TV_ZOOM_TICKS_MIN = 38400;
inline constexpr MIDITick TV_ZOOM_TICKS_MAX = 384000;

inline constexpr float GLOBAL_ZOOM_FACTOR = 1.5f;

struct PianoRollNavigation {
    float tick_pos = 0.0f;
    float key_pos = 21.0f;
    float zoom_ticks = 7680.0f;
    float zoom_keys = 88.0f;

    std::uint16_t curr_track = 0;

    float tick_pos_smoothed = 0.0f;
    float key_pos_smoothed = 21.0f;
    float zoom_ticks_smoothed = 7680.0f;
    float zoom_keys_smoothed = 88.0f;

    template <typename ChangeFn>
    void change_tick_pos(float new_tick_pos, ChangeFn change_fn) {
        tick_pos = new_tick_pos;
        change_fn(tick_pos);
    }

    void update_smoothed_values(float dt);

    [[nodiscard]] bool smoothed_values_needs_update() const;

    void zoom_ticks_by(float fac);
    void zoom_keys_by(float fac);
};

struct TrackViewNavigation {
    float tick_pos = 0.0f;
    float track_pos = 0.0f;
    float zoom_ticks = 3840.0f * 10.0f;
    float zoom_tracks = 10.0f;

    float tick_pos_smoothed = 0.0f;
    float track_pos_smoothed = 0.0f;
    float zoom_ticks_smoothed = 3840.0f * 10.0f;
    float zoom_tracks_smoothed = 10.0f;

    template <typename ChangeFn>
    void change_tick_pos(float new_tick_pos, ChangeFn change_fn) {
        tick_pos = new_tick_pos;
        change_fn(tick_pos);
    }

    void update_smoothed_values(float dt);

    [[nodiscard]] bool smoothed_values_needs_update() const;

    void zoom_ticks_by(float fac);
    void zoom_tracks_by(float fac);
};

}
