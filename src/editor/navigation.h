#pragma once

#include "editor/util.h"
#include "util/math/vector2.h"

// TODO: refactor this so each navigation uses a base class rather than being independent
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

    util::math::Vector2<float> midi_to_nav(std::pair<MIDITick, MIDIKey> midi_pos) {
        return {
            (static_cast<float>(midi_pos.first) - tick_pos_smoothed) / zoom_ticks_smoothed,
            (static_cast<float>(midi_pos.second) - key_pos_smoothed) / zoom_keys_smoothed
        };
    }

    std::pair<MIDITick, MIDIKey> nav_to_midi(util::math::Vector2<float> nav_pos) {
        return {
            static_cast<MIDITick>(nav_pos.x * zoom_ticks_smoothed + tick_pos_smoothed),
            static_cast<MIDIKey>(nav_pos.y * zoom_keys_smoothed + key_pos_smoothed)
        };
    }
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

    util::math::Vector2<float> midi_to_nav(std::pair<MIDITick, uint16_t> midi_pos) {
        return {
            (static_cast<float>(midi_pos.first) - tick_pos_smoothed) / zoom_ticks_smoothed,
            (static_cast<float>(midi_pos.second) - track_pos_smoothed) / zoom_tracks_smoothed
        };
    }

    std::pair<MIDITick, uint16_t> nav_to_midi(util::math::Vector2<float> nav_pos) {
        return {
            static_cast<MIDITick>(nav_pos.x * zoom_ticks_smoothed + tick_pos_smoothed),
            static_cast<uint16_t>(nav_pos.y * zoom_tracks_smoothed + track_pos_smoothed)
        };
    }
};

}
