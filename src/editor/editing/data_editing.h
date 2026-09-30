#pragma once

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

#include "app/editor_tool.h"
#include "app/view_settings.h"
#include "editor/actions.h"
#include "editor/navigation.h"
#include "editor/util.h"
#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::editor {

namespace data_edit_flags {
inline constexpr std::uint16_t DATA_EDIT_FLAGS_NONE = 0x0;
inline constexpr std::uint16_t DATA_EDIT_MOUSE_OVER_UI = 0x1;
inline constexpr std::uint16_t DATA_EDIT_MOUSE_DOWN_ON_UI = 0x2;
inline constexpr std::uint16_t DATA_EDIT_ANY_DIALOG_OPEN = 0x4;
inline constexpr std::uint16_t DATA_EDIT_DRAW_EDIT_LINE = 0x8;
inline constexpr std::uint16_t DATA_EDIT_CLICKED_IN_RECT = 0x10;
}

using DataNumType = std::int16_t;

struct DataEditMouseInfo {
    std::pair<MIDITick, DataNumType> mouse_data_pos{0, 0};
    std::pair<float, float> mouse_screen_pos{0.0f, 0.0f};
    std::pair<MIDITick, DataNumType> last_data_click_pos{0, 0};
    std::pair<float, float> last_screen_click_pos{0.0f, 0.0f};
};

class DataEditing {
public:
    DataEditing() = default;
    DataEditing(util::SharedPtr<std::vector<midi::MIDITrack>> tracks,
                util::SharedMutPtr<app::ViewSettings> view_settings,
                std::shared_ptr<app::EditorToolSettings> editor_tool,
                std::shared_ptr<EditorActions> editor_actions,
                util::SharedPtr<PianoRollNavigation> nav);

    void on_mouse_down();
    void on_mouse_move();
    void on_mouse_up();

    void update(const ViewRect& rect, float mouse_x, float mouse_y);

    void set_flag(std::uint16_t flag, bool value);
    [[nodiscard]] bool get_flag(std::uint16_t flag) const { return (flags_ & flag) != 0; }
    void enable_flag(std::uint16_t flag) { set_flag(flag, true); }
    void disable_flag(std::uint16_t flag) { flags_ &= static_cast<std::uint16_t>(~flag); }

    [[nodiscard]] std::pair<std::pair<float, float>, std::pair<float, float>>
    get_data_view_line_points() const;

private:
    [[nodiscard]] std::pair<MIDITick, DataNumType> screen_pos_to_data_pos(
        std::pair<float, float> screen_pos, const ViewRect& rect) const;

    [[nodiscard]] std::pair<float, float> data_pos_to_screen_pos(
        std::pair<MIDITick, DataNumType> data_pos, const ViewRect& rect) const;

    [[nodiscard]] DataNumType scaled_y_from_curr_data(float y) const;
    [[nodiscard]] float unscaled_y_from_curr_data(DataNumType y) const;

    void pencil_mouse_down();
    void pencil_mouse_move();
    void pencil_mouse_up();

    void eraser_mouse_down() {}
    void eraser_mouse_move() {}
    void eraser_mouse_up() {}
    void select_mouse_down() {}
    void select_mouse_move() {}
    void select_mouse_up() {}

    void set_note_velocities_ranged(MIDITick min_tick, std::uint8_t min_velocity,
                                    MIDITick max_tick, std::uint8_t max_velocity);

    void update_last_mouse_data_pos();
    [[nodiscard]] std::uint16_t get_curr_track() const;

    util::SharedPtr<std::vector<midi::MIDITrack>> tracks_;
    util::SharedMutPtr<app::ViewSettings> view_settings_;

    util::SharedPtr<PianoRollNavigation> nav_;

    std::shared_ptr<app::EditorToolSettings> editor_tool_;
    std::shared_ptr<EditorActions> editor_actions_;
    DataEditMouseInfo mouse_info_{};
    std::uint16_t flags_ = data_edit_flags::DATA_EDIT_FLAGS_NONE;
};

}
