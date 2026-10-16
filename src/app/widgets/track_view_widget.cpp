#include "app/widgets/track_view_widget.h"

#include <cmath>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include <imgui.h>

#include "app/main_window.h"
#include "app/rendering/track_view.h"
#include "app/widgets/timeline_nav.h"
#include "util/debugger.h"

namespace andromeda::app {

TrackViewWidget::TrackViewWidget(MainWindow& parent)
    : EditorWidget(parent), track_editing_(parent.editor_controller.get_track_editing()) {
    util::Debugger::log("Initializing track view renderer");
    auto renderer = std::make_shared<rendering::TrackViewRenderer>(&parent);

    // its ghost notes are per track, so they do not fit Renderer::set_ghost_notes
    renderer->set_ghost_notes(track_editing_->get_ghost_notes());
    renderer->set_ghost_note_offset(track_editing_->get_ghost_note_offset());

    target_renderer = std::move(renderer);
    component = track_editing_;
}

TickView TrackViewWidget::tick_view() const {
    const auto& nav = context.parent->track_nav;
    std::shared_lock lock(nav->mutex);
    return {nav->value.tick_pos_smoothed, nav->value.zoom_ticks_smoothed};
}

void TrackViewWidget::jump_to_tick(float pos) {
    const auto& nav = context.parent->track_nav;
    std::unique_lock lock(nav->mutex);
    nav->value.tick_pos = pos;
    nav->value.tick_pos_smoothed = pos;
}

void TrackViewWidget::zoom_by(float x_fac, float y_fac) {
    const auto& nav = context.parent->track_nav;
    std::unique_lock lock(nav->mutex);
    if (x_fac != 0.0f) {
        zoom_ticks_around_anchor(*context.parent, nav->value, x_fac);
    }
    if (y_fac != 0.0f) {
        nav->value.zoom_tracks_by(y_fac);
    }
}

void TrackViewWidget::handle_navigation() {
    const ImGuiIO& io = ImGui::GetIO();
    const float scroll_delta = io.MouseWheel;
    if (std::abs(scroll_delta) <= 0.001f) {
        return;
    }

    const bool alt_down = io.KeyAlt;
    const bool ctrl_down = io.KeyCtrl;

    MainWindow& parent = *context.parent;
    std::unique_lock lock(parent.track_nav->mutex);
    editor::TrackViewNavigation& n = parent.track_nav->value;

    const float move_by = scroll_delta;
    const float zoom_factor = std::pow(1.01f, scroll_delta);

    if (ctrl_down) {
        if (alt_down) {
            zoom_ticks_around_anchor(parent, n, zoom_factor);
        } else {
            const auto ppq_now = parent.get_ppq();

            float new_tick_pos =
                n.tick_pos + 2.0f * move_by * (n.zoom_ticks / static_cast<float>(ppq_now));
            if (new_tick_pos < 0.0f) {
                new_tick_pos = 0.0f;
            }

            n.tick_pos = new_tick_pos;
            n.change_tick_pos(new_tick_pos, [this](float time) {
                target_renderer->time_changed(static_cast<std::uint64_t>(time));
            });
        }
    } else {
        if (alt_down) {
            n.zoom_tracks_by(zoom_factor);
        } else {
            float new_track_pos = n.track_pos + (move_by > 0.0f ? -1.0f : 1.0f);
            if (new_track_pos < 0.0f) {
                new_track_pos = 0.0f;
            }
            n.track_pos = new_track_pos;
        }
    }
}

void TrackViewWidget::handle_input() {
    MainWindow& parent = *context.parent;

    using namespace editor::track_flags;
    const ImGuiIO& io = ImGui::GetIO();

    track_editing_->set_flag(TRACK_EDIT_MOUSE_OVER_UI, parent.mouse_over_ui);
    track_editing_->set_flag(TRACK_EDIT_ANY_DIALOG_OPEN,
                             parent.dialog_manager->is_any_dialog_shown());

    track_editing_->update();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        if (!parent.mouse_over_ui) {
            const auto mouse_track_pos = track_editing_->get_mouse_track_pos();
            if (parent.playhead) {
                parent.playhead->set_start(track_editing_->get_mouse_tick_pos_snapped());
            }
            parent.editor_controller.set_active_track(mouse_track_pos);

            // TODO: remove once everything uses editor_controller.get_active_track();
            std::shared_lock lock(parent.nav->mutex);
            parent.nav->value.curr_track = mouse_track_pos;
        }

        track_editing_->on_mouse_down();
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        track_editing_->on_right_mouse_down();
    }
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
        track_editing_->on_mouse_move();
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
        ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        track_editing_->on_mouse_up();
    }

    editor::TrackEditing::KeyState keys;
    const bool keys_free = !io.WantCaptureKeyboard;
    keys.track_up = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_UpArrow, false);
    keys.track_down = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_DownArrow, false);
    keys.del = keys_free && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    keys.copy = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false);
    keys.cut = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false);
    keys.paste = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false);
    track_editing_->on_key_down(keys);
}

void TrackViewWidget::draw_overlays() {
    draw_select_box();
    draw_playhead_line();
    draw_context_menu();
}

void TrackViewWidget::draw_select_box() const {
    if (!track_editing_->get_can_draw_selection_box()) {
        return;
    }

    const auto [tl, br] = track_editing_->get_selection_range_ui();
    const EditorToolSettings* tool_settings =
        context.parent->editor_controller.get_editor_tool_settings();
    const bool is_eraser = tool_settings->curr_tool == EditorTool::Eraser;

    const ImU32 fill = is_eraser ? IM_COL32(255, 50, 50, 40) : IM_COL32(100, 150, 255, 30);
    const ImU32 stroke = is_eraser ? IM_COL32(255, 80, 80, 255) : IM_COL32(120, 180, 255, 255);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->AddRectFilled({tl.x, tl.y}, {br.x, br.y}, fill, 2.0f);
    draw->AddRect({tl.x, tl.y}, {br.x, br.y}, stroke, 2.0f, 0, 1.5f);
}

void TrackViewWidget::draw_context_menu() {
    MainWindow& parent = *context.parent;

    if (ImGui::IsMouseReleased(ImGuiMouseButton_Right) && !parent.mouse_over_ui) {
        ImGui::OpenPopup("##trackview_context");
    }

    if (!ImGui::BeginPopup("##trackview_context")) {
        return;
    }

    const std::uint16_t right_clicked_track = track_editing_->get_right_clicked_track();
    bool should_close = false;

    if (ImGui::MenuItem("Insert track above")) {
        track_editing_->insert_track(right_clicked_track);
        should_close = true;
    }
    if (ImGui::MenuItem("Insert track below")) {
        track_editing_->insert_track(static_cast<std::uint16_t>(right_clicked_track + 1));
        should_close = true;
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Move track up")) {
        if (right_clicked_track != 0) {
            track_editing_->swap_tracks(right_clicked_track,
                                        static_cast<std::uint16_t>(right_clicked_track - 1));
        }
        should_close = true;
    }
    if (ImGui::MenuItem("Move track down")) {
        if (right_clicked_track + 1 >= track_editing_->get_used_track_count()) {
            track_editing_->insert_track(right_clicked_track);
        } else {
            track_editing_->swap_tracks(right_clicked_track,
                                        static_cast<std::uint16_t>(right_clicked_track + 1));
        }
        should_close = true;
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Remove Track")) {
        track_editing_->remove_right_clicked_track();
        should_close = true;
    }

    ImGui::Separator();

    if (ImGui::MenuItem("Decompose Track")) {
        track_editing_->decompose_track(right_clicked_track, true);
        should_close = true;
    }
    if (ImGui::IsItemHovered()) {
        ImGui::SetTooltip("Separates all channels in this track.");
    }

    parent.mouse_over_ui = true;

    if (should_close) {
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

}
