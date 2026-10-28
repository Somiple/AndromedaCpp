#include "ui/panels/panels.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <format>
#include <string>

#include "ui/app/editor_app.h"
#include "ui/app/main_window.h"
#include "ui/app/managers.h"
#include "ui/rendering/rendering.h"
#include "ui/widgets/toolbar_widgets.h"
#include "audio/audio_timer.h"
#include "audio/midi_audio.h"
#include "editor/editor_controller.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "editor/timeline/timeline.h"
#include "core/util.h"

// ---------------------------------------------------------------------------
// Playback panel group.
//
// Extracted from the former monolithic `app/ui/panels.cpp`. These panels all
// present or control the transport (playhead slider, autoscroll toggle, the
// large "fancy" transport read-out and the copy/undo side controls).
// ---------------------------------------------------------------------------

namespace andromeda::app {

using managers::PlaybackManager;
using toolbar::center_row_against;
using toolbar::EGUI_CHECKBOX_SIZE;
using toolbar::egui_checkbox;
using toolbar::egui_slider_int;
using toolbar::icon_button;

namespace {

void draw_chip(ImDrawList* dl, ImVec2 pos, ImVec2 size, ImU32 color, float alpha_mul = 1.0f) {
    dl->AddRectFilled(
        pos,
        pos + size,
        ImGui::GetColorU32(color, alpha_mul),
        6.0f
    );
}

} // namespace

void draw_panel_playback_buttons(MainWindow& parent) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));

    center_row_against(EGUI_CHECKBOX_SIZE);

    ImGui::TextUnformatted("Autoscroll");
    ImGui::SameLine();

    egui_checkbox("##autoscroll", &parent.settings().pr_autoscroll);

    ImGui::PopStyleVar(2);
}

void draw_panel_fancy_playback(MainWindow& parent)
{
    // refactor: the old `TODO: mutex` here is resolved as stale. this function reads the
    // project timeline and the audio timer for display only and holds no shared mutable
    // state of its own, so there is nothing for a lock to protect.
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const ImVec2 padding = ImGui::GetStyle().WindowPadding;
    const ImVec2 panel_padding = padding * 0.5f;

    const float window_height = ImGui::GetWindowHeight();
    float panel_height = window_height - padding.y * 2.0f;
    ImVec2 cursor_pos = ImGui::GetCursorScreenPos();

    const ImU32 text_color = ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Text]);
    ImFont* font = ImGui::GetFont();

    const float chip_opacity = 0.25f;
    const float chip_text_size = 40.0f;

    // get tempo map
    andromeda::editor::ProjectManager* project = parent.controller().get_project_manager();
    editor::timeline::Timeline* timeline = project->get_timeline();

    double secs = parent.get_editor_app()->get_timer()->elapsed();
    auto ticks = timeline->secs_to_tick(secs);

    // time, measure
    {
        const ImVec2 size = { 235.0f, panel_height };
        const ImVec2 inner_size = size - panel_padding * 2.0;
        const ImVec2 inner_pos = cursor_pos + panel_padding;

        draw_chip(
            draw_list,
            cursor_pos,
            size,
            text_color,
            chip_opacity
        );

        // The time (WIP)
        {
            std::string curr_time = util::format_duration(static_cast<double>(secs));
            const char* curr_time_cstr = curr_time.c_str();

            const float text_height = font->CalcTextSizeA(chip_text_size, FLT_MAX, 0.0f, curr_time_cstr).y;
            const ImVec2 text_pos = {
                inner_pos.x,
                cursor_pos.y + (size.y - text_height) * 0.5f
            };

            draw_list->AddText(
                font,
                chip_text_size,
                text_pos,
                text_color,
                curr_time_cstr
            );
        }

        // MEASURE, and measure:beat
        {
            // also right aligned but at a point
            const float region_size = 75.0f;

            const float start_x = inner_pos.x + inner_size.x - region_size;

            const char* measure_text = "MEASURE";
            const auto b = timeline->bar_at(ticks);
            const auto beat = (ticks - b.start) / b.beat_len + 1;      // 1-based, like the bar

            std::string measure_beat_tmp = std::format("{}:{}", b.bar + 1, beat);
            const float font_size = 17.0f;

            // top measure text
            ImVec2 text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, measure_text);
            ImVec2 text_pos = {
                start_x,
                inner_pos.y
            };

            draw_list->AddText(
                font,
                font_size,
                text_pos,
                ImGui::GetColorU32(text_color, 0.6f),
                measure_text
            );

            // measure:beat text
            text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, measure_beat_tmp.c_str());
            text_pos = {
                start_x,
                inner_pos.y + inner_size.y - text_size.y
            };

            draw_list->AddText(
                font,
                font_size,
                text_pos,
                text_color,
                measure_beat_tmp.c_str()
            );
        }

        ImGui::Dummy(size);
    }

    ImGui::SameLine();
    cursor_pos = ImGui::GetCursorScreenPos();

    // time sig and bpm
    {
        const ImVec2 size = { 150.0f, panel_height };
        const ImVec2 inner_size = size - panel_padding * 2.0;
        const ImVec2 inner_pos = cursor_pos + panel_padding;

        draw_chip(
            draw_list,
            cursor_pos,
            size,
            text_color,
            chip_opacity
        );

        const float half_point = inner_pos.x + inner_size.x * 0.5f;

        // separator line
        draw_list->AddLine(
            { half_point, cursor_pos.y },
            { half_point, cursor_pos.y + size.y },
            ImGui::GetColorU32(text_color, 0.6f),
            2.0f
        );

        // time signature and bpm
        {
            const char* time_sig_tmp = "4/4";
            std::string bpm_str = std::to_string(std::llroundf(timeline->bpm_at(ticks)));
            const char* bpm_cstr = bpm_str.c_str();

            ImVec2 text_size = font->CalcTextSizeA(chip_text_size, FLT_MAX, 0.0f, time_sig_tmp);
            ImVec2 text_pos = {
                inner_pos.x + (inner_size.x * 0.5f - text_size.x) * 0.5f,
                inner_pos.y + (inner_size.y - text_size.y) * 0.5f
            };

            draw_list->AddText(
                font,
                chip_text_size,
                text_pos,
                text_color,
                time_sig_tmp
            );

            text_size = font->CalcTextSizeA(chip_text_size, FLT_MAX, 0.0f, bpm_cstr);
            text_pos = {
                inner_pos.x + inner_size.x * 0.5f + (inner_size.x * 0.5f - text_size.x) * 0.5f,
                inner_pos.y + (inner_size.y - text_size.y) * 0.5f
            };

            draw_list->AddText(
                font,
                chip_text_size,
                text_pos,
                text_color,
                bpm_cstr
            );
        }

        ImGui::Dummy(size);
    }

    ImGui::SameLine();

    // Local separator lambda (unchanged): it draws a full-height rule rather than
    // the compact toolbar separator, so it cannot reuse toolbar_separator().
    auto vertical_separator = [&]() {
        ImVec2 pos = ImGui::GetCursorScreenPos();

        draw_list->AddLine(
            { pos.x, pos.y - padding.y },
            { pos.x, pos.y - padding.y + window_height },
            ImGui::GetColorU32(text_color, 0.1f),
            1.5f
        );

        ImGui::Dummy({ 2.0f, window_height });
        ImGui::SameLine();
        cursor_pos = ImGui::GetCursorScreenPos();
    };

    vertical_separator();

    // refactor: the empty "playback buttons" placeholder left by the panel split was removed;
    // the transport buttons are drawn by draw_panel_playback_buttons above.
}

void draw_panel_side_controls(MainWindow& parent) {
    const ImageResources& images = parent.images();
    editor::EditorController* controller = &parent.controller();

    if (icon_button("##copy", images.get_image_handle("copy"), false, controller->can_copy(), "C")) {
        controller->copy();
    }
    if (icon_button("##cut", images.get_image_handle("cut"), false, controller->can_copy(), "X")) {
        controller->cut();
    }

    if (icon_button("##paste", images.get_image_handle("paste"), false, controller->can_paste(), "V")) {
        controller->paste();
    }

    ImGui::Separator();

    if (icon_button("##undo", images.get_image_handle("undo"), false, controller->can_undo(), "U")) {
        controller->undo();
    }
    if (icon_button("##redo", images.get_image_handle("redo"), false, controller->can_redo(), "R")) {
        controller->redo();
    }
}

void draw_panel_playhead_ui(MainWindow& parent) {
    //   fixed: the knob now uses the playhead line's view and position, so zoom moves both alike
    // fixed again: central view now provides "true_tick_view", so we can use that instead
    const auto tick_view = parent.get_central_tick_view();
    const auto min_tick = static_cast<int>(tick_view.pos);
    const auto max_tick = static_cast<int>(tick_view.pos + tick_view.zoom);

    // span the roll's note area exactly, as the bar numbers do, not the padded content region
    const float kb_width = parent.get_keyboard_width();
    const float slider_x = ImGui::GetWindowPos().x + kb_width;
    const float slider_w = std::max(1.0f, ImGui::GetWindowSize().x - kb_width);
    ImGui::SetCursorScreenPos({slider_x, ImGui::GetCursorScreenPos().y});

    int playhead_time = static_cast<int>(parent.get_editor_app()->get_playback_manager()->get_playhead_tick_pos());

    if (egui_slider_int("##playhead", &playhead_time, static_cast<int>(min_tick),
                        static_cast<int>(max_tick), slider_w)) {
        const editor::Timeline* timeline = parent.controller().get_project_manager()->get_timeline();

        const editor::MIDITick t = static_cast<editor::MIDITick>(std::max(0, playhead_time));
        const editor::MIDITick snapped = timeline->snap_nearest(t);

        // a seek while autoscrolling moves the start the view is measured from; hold the
        // view where it is on screen so only the playhead moves
        auto* playback_manager = parent.get_editor_app()->get_playback_manager();
        auto* audio = parent.get_editor_app()->get_audio_engine();
        if (audio->get_current_engine()->is_playing() && parent.settings().pr_autoscroll) {
            const float view_min = static_cast<float>(min_tick);
            if (parent.render_type() == RenderType::TrackView) {
                std::unique_lock lock(parent.track_navigation()->mutex);
                parent.track_navigation()->value.tick_pos = view_min;
                parent.track_navigation()->value.tick_pos_smoothed = view_min;
            } else {
                std::unique_lock lock(parent.navigation()->mutex);
                parent.navigation()->value.tick_pos = view_min;
                parent.navigation()->value.tick_pos_smoothed = view_min;
            }
        }

        bool was_playing = playback_manager->is_playing();
        if (was_playing) playback_manager->stop_playback();
        playback_manager->set_playback_start(snapped);
        if (was_playing) playback_manager->start_playback();

        if (parent.render_type() == RenderType::TrackView) {
            std::unique_lock lock(parent.navigation()->mutex);
            parent.navigation()->value.tick_pos =
                static_cast<float>(snapped > 960 ? snapped - 960 : 0);
        }
    }
}

} // namespace andromeda::app
