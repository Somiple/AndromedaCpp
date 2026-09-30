#include "editor/editing/data_editing.h"

#include <algorithm>
#include <utility>

namespace andromeda::editor {

using namespace data_edit_flags;
using app::EditorTool;
using app::VS_PianoRoll_DataViewState;

DataEditing::DataEditing(util::SharedPtr<std::vector<midi::MIDITrack>> tracks,
                         util::SharedMutPtr<app::ViewSettings> view_settings,
                         std::shared_ptr<app::EditorToolSettings> editor_tool,
                         std::shared_ptr<EditorActions> editor_actions,
                         util::SharedPtr<PianoRollNavigation> nav)
    : tracks_(std::move(tracks)),
      view_settings_(std::move(view_settings)),
      nav_(std::move(nav)),
      editor_tool_(std::move(editor_tool)),
      editor_actions_(std::move(editor_actions)) {}

void DataEditing::on_mouse_down() {
    if (get_flag(DATA_EDIT_MOUSE_OVER_UI)) {
        enable_flag(DATA_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    if (get_flag(DATA_EDIT_ANY_DIALOG_OPEN)) {
        return;
    }

    switch (editor_tool_->curr_tool) {
    case EditorTool::Pencil:   pencil_mouse_down(); break;
    case EditorTool::Eraser:   eraser_mouse_down(); break;
    case EditorTool::Selector: select_mouse_down(); break;
    }

    enable_flag(DATA_EDIT_CLICKED_IN_RECT);
}

void DataEditing::on_mouse_move() {
    if (get_flag(DATA_EDIT_MOUSE_DOWN_ON_UI | DATA_EDIT_CLICKED_IN_RECT)) {
        return;
    }

    if (get_flag(DATA_EDIT_ANY_DIALOG_OPEN | DATA_EDIT_MOUSE_OVER_UI)) {
        return;
    }

    switch (editor_tool_->curr_tool) {
    case EditorTool::Pencil:   pencil_mouse_move(); break;
    case EditorTool::Eraser:   eraser_mouse_move(); break;
    case EditorTool::Selector: select_mouse_move(); break;
    }
}

void DataEditing::on_mouse_up() {
    if (get_flag(DATA_EDIT_MOUSE_DOWN_ON_UI)) {
        disable_flag(DATA_EDIT_MOUSE_DOWN_ON_UI);
        return;
    }

    if (!get_flag(DATA_EDIT_CLICKED_IN_RECT)) {
        return;
    }
    // fixed rust bug: never cleared, so any later release ran the tool's mouse-up
    disable_flag(DATA_EDIT_CLICKED_IN_RECT);

    if (get_flag(DATA_EDIT_MOUSE_OVER_UI | DATA_EDIT_ANY_DIALOG_OPEN)) {
        return;
    }

    switch (editor_tool_->curr_tool) {
    case EditorTool::Pencil:   pencil_mouse_up(); break;
    case EditorTool::Eraser:   eraser_mouse_up(); break;
    case EditorTool::Selector: select_mouse_up(); break;
    }
}

void DataEditing::update(const ViewRect& rect, float mouse_x, float mouse_y) {
    mouse_info_.mouse_data_pos = screen_pos_to_data_pos({mouse_x, mouse_y}, rect);
    mouse_info_.mouse_screen_pos = {mouse_x, mouse_y};
}

std::pair<MIDITick, DataNumType> DataEditing::screen_pos_to_data_pos(
    std::pair<float, float> screen_pos, const ViewRect& rect) const {
    const float screen_x_norm = (screen_pos.first - rect.left) / rect.width;

    // fixed rust bug: used a hardcoded 21px row and 200px height instead of the strip rect
    const float screen_y_norm = 1.0f - (screen_pos.second - rect.top) / rect.height;

    MIDITick screen_x_tick = 0;
    {
        std::shared_lock lock(nav_->mutex);
        screen_x_tick = static_cast<MIDITick>(screen_x_norm * nav_->value.zoom_ticks_smoothed +
                                              nav_->value.tick_pos_smoothed);
    }

    const DataNumType screen_y_data = scaled_y_from_curr_data(screen_y_norm);

    return {screen_x_tick, screen_y_data};
}

std::pair<float, float> DataEditing::data_pos_to_screen_pos(
    std::pair<MIDITick, DataNumType> data_pos, const ViewRect& rect) const {
    float data_x_norm = 0.0f;
    {
        std::shared_lock lock(nav_->mutex);
        // fixed rust bug: divided x by the key zoom instead of the tick zoom
        data_x_norm = (static_cast<float>(data_pos.first) - nav_->value.tick_pos_smoothed) /
                      nav_->value.zoom_ticks_smoothed;
    }

    const float data_y_norm = unscaled_y_from_curr_data(data_pos.second);

    const float data_x_scr = data_x_norm * rect.width + rect.left;
    const float data_y_scr = (1.0f - data_y_norm) * rect.height + rect.top;

    return {data_x_scr, data_y_scr};
}

DataNumType DataEditing::scaled_y_from_curr_data(float y) const {
    std::lock_guard lock(view_settings_->mutex);

    switch (view_settings_->value.pr_dataview_state) {
    case VS_PianoRoll_DataViewState::NoteVelocities:
        return std::clamp(static_cast<DataNumType>(y * 127.0f), static_cast<DataNumType>(0),
                          static_cast<DataNumType>(127));
    case VS_PianoRoll_DataViewState::PitchBend:
        return std::clamp(static_cast<DataNumType>((y * 2.0f - 1.0f) * 8192.0f),
                          static_cast<DataNumType>(-8192), static_cast<DataNumType>(8191));
    default:
        return 0;
    }
}

float DataEditing::unscaled_y_from_curr_data(DataNumType y) const {
    std::lock_guard lock(view_settings_->mutex);

    switch (view_settings_->value.pr_dataview_state) {
    case VS_PianoRoll_DataViewState::NoteVelocities:
        return static_cast<float>(y) / 127.0f;
    case VS_PianoRoll_DataViewState::PitchBend:
        return static_cast<float>(y) / 8192.0f * 0.5f + 0.5f;
    default:
        return 0.0f;
    }
}

void DataEditing::pencil_mouse_down() {
    update_last_mouse_data_pos();
    enable_flag(DATA_EDIT_DRAW_EDIT_LINE);
}

void DataEditing::pencil_mouse_move() {}

void DataEditing::pencil_mouse_up() {
    disable_flag(DATA_EDIT_DRAW_EDIT_LINE);

    auto dp1 = mouse_info_.last_data_click_pos;
    auto dp2 = mouse_info_.mouse_data_pos;
    if (dp1.first > dp2.first) {
        std::swap(dp1, dp2);
    }

    VS_PianoRoll_DataViewState dataview_state{};
    {
        std::lock_guard lock(view_settings_->mutex);
        dataview_state = view_settings_->value.pr_dataview_state;
    }

    switch (dataview_state) {
    case VS_PianoRoll_DataViewState::NoteVelocities:
        set_note_velocities_ranged(dp1.first, static_cast<std::uint8_t>(dp1.second), dp2.first,
                                   static_cast<std::uint8_t>(dp2.second));
        break;
    case VS_PianoRoll_DataViewState::PitchBend:
        break;
    case VS_PianoRoll_DataViewState::Hidden:
        break;
    }
}

void DataEditing::set_note_velocities_ranged(MIDITick min_tick, std::uint8_t min_velocity,
                                             MIDITick max_tick, std::uint8_t max_velocity) {
    const std::uint16_t curr_track = get_curr_track();

    std::vector<std::size_t> ids;
    std::vector<std::int8_t> vel_changes;

    {
        std::unique_lock lock(tracks_->mutex);
        if (static_cast<std::size_t>(curr_track) >= tracks_->value.size()) {
            return;
        }

        std::vector<midi::Note>& notes = tracks_->value[curr_track].get_notes_mut();

        const auto by_start = [](const midi::Note& n, MIDITick t) { return n.get_start() < t; };

        const std::size_t note_id_min = static_cast<std::size_t>(
            std::lower_bound(notes.begin(), notes.end(), min_tick, by_start) - notes.begin());
        const std::size_t note_id_max = static_cast<std::size_t>(
            std::lower_bound(notes.begin(), notes.end(), max_tick, by_start) - notes.begin());

        for (std::size_t id = note_id_min; id < note_id_max; ++id) {
            ids.push_back(id);
        }
        vel_changes.reserve(ids.size());

        for (const std::size_t id : ids) {
            midi::Note& note = notes[id];
            const MIDITick note_tick = note.get_start();

            // fixed rust bug: divided by zero on a zero-width drag
            const float vel_factor =
                max_tick > min_tick ? static_cast<float>(note_tick - min_tick) /
                                          static_cast<float>(max_tick - min_tick)
                                    : 0.0f;
            const auto vel_mix = static_cast<std::uint8_t>(
                (1.0f - vel_factor) * static_cast<float>(min_velocity) +
                vel_factor * static_cast<float>(max_velocity));

            const std::uint8_t old_vel = note.get_velocity();
            const std::uint8_t new_vel = vel_mix;
            note.velocity = new_vel;

            vel_changes.push_back(static_cast<std::int8_t>(static_cast<std::int8_t>(new_vel) -
                                                           static_cast<std::int8_t>(old_vel)));
        }
    }

    editor_actions_->register_action(
        VelocityChange{std::move(ids), std::move(vel_changes), curr_track});
}

void DataEditing::set_flag(std::uint16_t flag, bool value) {
    const auto mask = static_cast<std::uint16_t>(value ? 0xFFFF : 0x0000);
    flags_ = static_cast<std::uint16_t>((flags_ & static_cast<std::uint16_t>(~flag)) |
                                        (mask & flag));
}

void DataEditing::update_last_mouse_data_pos() {
    mouse_info_.last_data_click_pos = mouse_info_.mouse_data_pos;
    mouse_info_.last_screen_click_pos = mouse_info_.mouse_screen_pos;
}

std::pair<std::pair<float, float>, std::pair<float, float>>
DataEditing::get_data_view_line_points() const {
    const auto point_1 = mouse_info_.last_screen_click_pos;
    const auto point_2 = mouse_info_.mouse_screen_pos;

    if (point_1.first > point_2.first) {
        return {point_2, point_1};
    }
    return {point_1, point_2};
}

std::uint16_t DataEditing::get_curr_track() const {
    std::shared_lock lock(nav_->mutex);
    return nav_->value.curr_track;
}

}
