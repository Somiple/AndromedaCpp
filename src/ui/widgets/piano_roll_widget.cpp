#include "ui/widgets/piano_roll_widget.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <shared_mutex>

#include <imgui.h>

#include "ui/app/main_window.h"
#include "ui/app/editor_app.h"
#include "ui/app/managers.h"
#include "ui/rendering/piano_roll.h"
#include "ui/widgets/timeline_nav.h"
#include "editor/settings/editor_settings.h"
#include "core/debugger.h"

namespace andromeda::app {

PianoRollWidget::PianoRollWidget(MainWindow& parent)
    : EditorWidget(parent), note_editing_(parent.controller().get_note_editing()) {
    util::Debugger::log("Initializing piano roll renderer");
    target_renderer = std::make_shared<rendering::PianoRollRenderer>(&parent);
    target_renderer->set_ghost_notes(note_editing_->get_ghost_notes());

    component = note_editing_;
}

rendering::PianoRollRenderer& PianoRollWidget::renderer() const {
    return static_cast<rendering::PianoRollRenderer&>(*target_renderer);
}

TickView PianoRollWidget::tick_view() const {
    const auto& nav = context.parent->navigation();
    std::shared_lock lock(nav->mutex);
    return {nav->value.tick_pos_smoothed, nav->value.zoom_ticks_smoothed};
}

TickView PianoRollWidget::true_tick_view() const {
    MainWindow* main_window = context.parent;
    ViewSettings* view_settings = &main_window->settings();
    
    const auto& nav = main_window->navigation();

    // TODO: maybe add a MIDITick version of the smoothed tick pos so no floating point precision errors can happen,,
    float start_tick = nav->value.tick_pos_smoothed;
    float visible_ticks = nav->value.zoom_ticks_smoothed;

    if (view_settings->pr_autoscroll) {
        const auto* app = main_window->get_editor_app();
        const auto* timer = app->get_timer();
        const auto* playback = app->get_playback_manager();
        const auto* timeline = main_window->controller().get_project_manager()->get_timeline();
        if (playback->is_playing()) {
            start_tick += (float)(timeline->secs_to_tick(timer->elapsed())
                - timeline->secs_to_tick(timer->get_start_time()));
        }
    }

    return TickView{ .pos = std::fmax(start_tick, 0.0f), .zoom = visible_ticks };
}

void PianoRollWidget::jump_to_tick(float pos) {
    const auto& nav = context.parent->navigation();
    std::unique_lock lock(nav->mutex);
    nav->value.tick_pos = pos;
    nav->value.tick_pos_smoothed = pos;
}

void PianoRollWidget::zoom_by(float x_fac, float y_fac) {
    const auto& nav = context.parent->navigation();
    std::unique_lock lock(nav->mutex);
    if (x_fac != 0.0f) {
        zoom_ticks_around_anchor(*context.parent->get_editor_app(), nav->value, x_fac);
    }
    if (y_fac != 0.0f) {
        nav->value.zoom_keys_by(y_fac);
    }
}

float PianoRollWidget::keyboard_width() const {
    return editor::PR_KEYBOARD_WIDTH * context.parent->app_scale();
}

void PianoRollWidget::handle_navigation() {
    const ImGuiIO& io = ImGui::GetIO();
    const float scroll_delta = io.MouseWheel * 10.0f;
    if (std::abs(scroll_delta) <= 0.001f) {
        return;
    }

    const bool alt_down = io.KeyAlt;
    const bool ctrl_down = io.KeyCtrl;

    MainWindow& parent = *context.parent;
    std::unique_lock lock(parent.navigation()->mutex);
    editor::PianoRollNavigation& n = parent.navigation()->value;

    const float move_by = scroll_delta;

    const float zoom_factor = std::pow(1.01f, -scroll_delta);

    if (ctrl_down) {
        if (alt_down) {
            zoom_ticks_around_anchor(*parent.get_editor_app(), n, zoom_factor);
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
            n.zoom_keys_by(zoom_factor);
        } else {
            float new_key_pos = n.key_pos + move_by * (n.zoom_keys / 128.0f);
            if (new_key_pos < 0.0f) {
                new_key_pos = 0.0f;
            }
            if (new_key_pos + n.zoom_keys > 128.0f) {
                new_key_pos = 128.0f - n.zoom_keys;
            }
            n.key_pos = new_key_pos;
        }
    }
}

void PianoRollWidget::handle_input() {
    MainWindow& parent = *context.parent;

    using namespace editor::note_edit_flags;
    const ImGuiIO& io = ImGui::GetIO();

    note_editing_->set_flag(NOTE_EDIT_MOUSE_OVER_UI, parent.is_mouse_over_ui());
    note_editing_->set_flag(NOTE_EDIT_ANY_DIALOG_OPEN,
                            parent.dialogs().is_any_dialog_shown());

    note_editing_->update();

    // the note under the mouse is previewed while placing or dragging (it was never wired up)
    audio::AudioEngine* audio_engine = parent.get_editor_app()->get_audio_engine()->get_current_engine();
    ToolBarSettings* toolbar_settings = parent.controller().get_toolbar_settings();
    const bool can_preview = audio_engine != nullptr && toolbar_settings != nullptr;
    const auto preview_key = [&] {
        return static_cast<std::uint8_t>(
            std::min<int>(note_editing_->mouse_info().mouse_midi_pos.second, 127));
    };
    const auto preview_channel = [&] {
        return static_cast<std::uint8_t>(std::clamp(toolbar_settings->note_channel - 1, 0, 15));
    };
    const auto preview_velocity = [&] {
        return static_cast<std::uint8_t>(std::clamp(toolbar_settings->note_velocity, 0, 127));
    };

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        note_editing_->on_mouse_down();
        if (can_preview && note_editing_->get_flag(NOTE_EDIT_SYNTH_PLAY)) {
            audio_engine->start_play_at_mouse(preview_key(), preview_channel(),
                                              preview_velocity());
        }
    }
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        note_editing_->on_right_mouse_down();
    }
    if (io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f) {
        note_editing_->on_mouse_move();
    }
    if (can_preview && ImGui::IsMouseDown(ImGuiMouseButton_Left) &&
        note_editing_->get_flag(NOTE_EDIT_SYNTH_PLAY)) {
        audio_engine->update_play_at_mouse(preview_key(), preview_channel(), preview_velocity());
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
        ImGui::IsMouseReleased(ImGuiMouseButton_Right)) {
        if (can_preview && ImGui::IsMouseReleased(ImGuiMouseButton_Left) &&
            note_editing_->get_flag(NOTE_EDIT_SYNTH_PLAY)) {
            audio_engine->stop_play_at_mouse(preview_key(), preview_channel());
        }
        note_editing_->on_mouse_up();
    }

    editor::NoteEditing::KeyState keys;
    const bool keys_free = !io.WantCaptureKeyboard;
    keys.copy = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C, false);
    keys.cut = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_X, false);
    keys.paste = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V, false);
    keys.duplicate = keys_free && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D, false);
    keys.del = keys_free && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    note_editing_->on_key_down(keys);

    switch (note_editing_->get_cursor()) {
    case editor::EditCursor::ResizeHorizontal:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
        break;
    case editor::EditCursor::Move:
        ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
        break;
    case editor::EditCursor::Crosshair:
        ImGui::SetMouseCursor(ImGuiMouseCursor_Arrow);
        break;
    case editor::EditCursor::Default:
        break;
    }
}

void PianoRollWidget::draw_overlays() {
    note_editing_->draw_overlays();
    draw_playhead_line();
}

void PianoRollWidget::draw_select_box() const {
    util::Debugger::log_warning("DEPRECATED: use overlay framework to draw selection boxes");
}

}
