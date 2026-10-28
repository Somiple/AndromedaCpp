#include "ui/panels/panels.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_stdlib.h>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "ui/app/main_window.h"
#include "ui/style/note_colors.h"
#include "ui/widgets/toolbar_widgets.h"
#include "editor/editor_controller.h"
#include "editor/project/project_manager.h"
#include "editor/settings/editor_settings.h"

// ---------------------------------------------------------------------------
// Track-list panel group.
//
// Extracted from the former monolithic `app/ui/panels.cpp`. Rows, mute/solo
// buttons, the rename/delete context menu and the channel swatch grid all
// belong to the track list, so they now live in this translation unit.
// ---------------------------------------------------------------------------

namespace andromeda::app {

namespace {

struct TrackListPopupState {
    uint16_t rename_track_index = 0;
    std::string rename_track_buffer = "";
    bool open_rename_popup = false;
};

struct TrackListStyle {
    ImFont* font;
    ImU32 text_color;
    ImU32 muted_text_color;
    ImU32 selected_track_color;
    ImVec2 row_padding;
    ImVec2 row_size;
    ImVec2 mute_size;
    ImVec2 solo_size;
    float spacing;
    float button_y;
    float tnum_text_size;
    float tnum_text_height;
    float tnam_text_size;
    float tnam_text_height;
};

TrackListStyle get_track_list_style() {
    ImFont* font = ImGui::GetFont();
    const ImU32 text_color = ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_Text]);

    TrackListStyle style{
        .font = font,
        .text_color = text_color,
        .muted_text_color = ImGui::GetColorU32(text_color, 0.5f),
        .selected_track_color = ImGui::GetColorU32(ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_CheckMark]), 0.5f),
        .row_padding = { 4.0f, 2.0f },
        .row_size = { ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.0f, 30.0f },
        .mute_size = ImGui::CalcTextSize("M") + ImGui::GetStyle().FramePadding * 2.0f,
        .solo_size = ImGui::CalcTextSize("S") + ImGui::GetStyle().FramePadding * 2.0f,
        .spacing = ImGui::GetStyle().ItemSpacing.x,
        .button_y = 0.0f,
        .tnum_text_size = 13.0f,
        .tnum_text_height = font->CalcTextSizeA(13.0f, FLT_MAX, 0.0f, "#").y,
        .tnam_text_size = 15.0f,
        .tnam_text_height = font->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, "#").y
    };

    style.button_y = (style.row_size.y - style.mute_size.y) * 0.5f;

    return style;
}

void draw_track_row_background(ImDrawList* draw_list, const ImVec2& row_pos, const TrackListStyle& style, bool selected) {
    if (!selected) {
        return;
    }

    draw_list->AddRectFilled(row_pos, row_pos + style.row_size, style.selected_track_color, 4.0f);
}

void draw_track_row_labels(ImDrawList* draw_list, const midi::MIDITrack& track, uint16_t track_index, const ImVec2& row_pos, const TrackListStyle& style) {
    const ImVec2 inner_pos = row_pos + style.row_padding;
    const std::string track_num = std::to_string(track_index + 1);

    const ImVec2 track_num_pos = inner_pos + ImVec2{
        0.0f,
        (style.row_size.y - style.tnum_text_height) * 0.5f
    };

    draw_list->AddText(style.font, style.tnum_text_size, track_num_pos, ImGui::GetColorU32(style.text_color, 0.7f), track_num.c_str());

    const ImVec2 track_name_pos = inner_pos + ImVec2{
        15.0f,
        (style.row_size.y - style.tnam_text_height) * 0.5f
    };

    std::string track_name = "Track " + std::to_string(static_cast<int>(track_index) + 1);
    if (track.name.has_value()) track_name = *track.name;
    draw_list->AddText(style.font, style.tnam_text_size, track_name_pos, track.muted ? style.muted_text_color : style.text_color, track_name.c_str());
}

std::pair<ImVec2, ImVec2> get_track_button_positions(const ImVec2& row_pos, const TrackListStyle& style) {
    const ImVec2 solo_pos{
        row_pos.x + style.row_size.x - style.solo_size.x - style.row_padding.x,
        row_pos.y + style.button_y
    };

    const ImVec2 mute_pos{
        solo_pos.x - style.spacing - style.mute_size.x,
        row_pos.y + style.button_y
    };

    return { mute_pos, solo_pos };
}

void draw_track_buttons(MainWindow& parent, midi::MIDITrack& track, uint16_t track_index, const ImVec2& row_pos, const TrackListStyle& style) {
    auto* project_manager = parent.controller().get_project_manager();
    auto [mute_pos, solo_pos] = get_track_button_positions(row_pos, style);
    const std::string track_id = std::to_string(track_index);

    ImGui::SetCursorScreenPos(mute_pos);
    if (ImGui::Button(("M###m" + track_id).c_str())) {
        project_manager->get_project_data().set_track_muted(track_index, !track.muted);
    }

    ImGui::SetCursorScreenPos(solo_pos);
    if (ImGui::Button(("S###s" + track_id).c_str())) {
        project_manager->get_project_data().solo_track(track_index, true);
    }
}

bool draw_track_row_hitbox(editor::EditorController& controller, uint16_t track_index, const ImVec2& row_pos, const TrackListStyle& style) {
    auto [mute_pos, solo_pos] = get_track_button_positions(row_pos, style);
    const float row_click_width = mute_pos.x - row_pos.x - style.spacing;

    ImGui::SetCursorScreenPos(row_pos);

    if (ImGui::InvisibleButton(("track_row###" + std::to_string(track_index)).c_str(), { row_click_width, style.row_size.y })) {
        controller.set_active_track(track_index);
    }

    return ImGui::IsItemClicked(ImGuiMouseButton_Right);
}

void draw_track_context_menu(editor::EditorController& controller, midi::MIDITrack& track, uint16_t track_index, TrackListPopupState& popup_state) {
    const std::string popup_id = "track_context###" + std::to_string(track_index);

    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) {
        controller.set_active_track(track_index);
        ImGui::OpenPopup(popup_id.c_str());
    }

    if (!ImGui::BeginPopup(popup_id.c_str())) {
        return;
    }

    if (ImGui::MenuItem("Rename")) {
        popup_state.rename_track_index = track_index;
        popup_state.rename_track_buffer = track.name.has_value() ? *track.name : "";
        popup_state.open_rename_popup = true;
        ImGui::CloseCurrentPopup();
    }

    if (ImGui::MenuItem("Delete")) {
        controller.remove_track(track_index);
    }

    ImGui::EndPopup();
}

void draw_rename_track_popup(std::vector<midi::MIDITrack>& tracks, TrackListPopupState& popup_state) {
    if (popup_state.open_rename_popup) {
        ImGui::OpenPopup("Rename track");
        popup_state.open_rename_popup = false;
    }

    if (!ImGui::BeginPopupModal("Rename track", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
        return;
    }

    ImGui::Text("New name:");
    ImGui::SameLine();

    if (ImGui::InputText("##track_name", &popup_state.rename_track_buffer, ImGuiInputTextFlags_EnterReturnsTrue)) {
        tracks[popup_state.rename_track_index].name = popup_state.rename_track_buffer;
        ImGui::CloseCurrentPopup();
    }

    ImGui::Separator();

    if (ImGui::Button("Rename")) {
        std::optional<std::string> new_name = std::nullopt;
        if (!popup_state.rename_track_buffer.empty()) new_name = popup_state.rename_track_buffer;
        tracks[popup_state.rename_track_index].name = std::move(new_name);
        ImGui::CloseCurrentPopup();
    }

    ImGui::SameLine();

    if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        ImGui::CloseCurrentPopup();
    }

    ImGui::EndPopup();
}

void draw_track_row(MainWindow& parent, midi::MIDITrack& track, uint16_t track_index, uint16_t current_track, const TrackListStyle& style, TrackListPopupState& popup_state) {
    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    const ImVec2 row_pos = ImGui::GetCursorScreenPos();

    draw_track_row_background(draw_list, row_pos, style, track_index == current_track);
    draw_track_row_labels(draw_list, track, track_index, row_pos, style);
    draw_track_buttons(parent, track, track_index, row_pos, style);
    draw_track_row_hitbox(parent.controller(), track_index, row_pos, style);
    draw_track_context_menu(parent.controller(), track, track_index, popup_state);

    ImGui::SetCursorScreenPos(row_pos);
    ImGui::Dummy(style.row_size);
}

constexpr const int SWATCHES_PER_ROW = 8;
constexpr int SWATCH_ROWS = (16 + SWATCHES_PER_ROW - 1) / SWATCHES_PER_ROW;

float get_track_controls_height() {
    const ImGuiStyle& style = ImGui::GetStyle();

    const float frame_width = ImGui::GetWindowWidth();
    const float content_width = frame_width - style.WindowPadding.x * 2.0f;

    const float total_horizontal_spacing = style.ItemSpacing.x * static_cast<float>(SWATCHES_PER_ROW - 1);

    const float swatch_width = (content_width - total_horizontal_spacing) / static_cast<float>(SWATCHES_PER_ROW);

    const float swatch_height = swatch_width;
    const float separator_height = style.ItemSpacing.y + style.SeparatorTextBorderSize;

    return
        // Separator + "Channels"
        separator_height +
        ImGui::GetTextLineHeight() +
        // text -> first row
        style.ItemSpacing.y +
        // swatch grid
        swatch_height * SWATCH_ROWS +
        style.ItemSpacing.y * (SWATCH_ROWS - 1) +
        // grid -> separator
        separator_height +
        // buttons lol
        ImGui::GetFrameHeight();
}

void draw_track_list_scroll_region(MainWindow& parent, TrackListPopupState& popup_state, TrackListStyle& style) {
    auto* editor_controller = &parent.controller();
    auto* project_manager = editor_controller->get_project_manager();
    auto& tracks = *project_manager->get_tracks();
    const uint16_t current_track = editor_controller->get_active_track();

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, { 0.0, 0.0 });
    if (!ImGui::BeginChild("track_list", ImVec2{ 0.0f,-get_track_controls_height()}, true,
        ImGuiWindowFlags_NoScrollbar)) {
        ImGui::EndChild();
        return;
    }
    ImGui::PopStyleVar();

    uint16_t track_index = 0;

    // we set the row width here
    style.row_size.x = ImGui::GetWindowWidth();

    for (midi::MIDITrack& track : tracks) {
        draw_track_row(parent, track, track_index, current_track, style, popup_state);
        ++track_index;
    }

    draw_rename_track_popup(tracks, popup_state);

    ImGui::EndChild();
}

void draw_track_controls(MainWindow& parent) {
    auto* editor_controller = &parent.controller();
    uint16_t active_track = editor_controller->get_active_track();
    uint8_t active_channel = editor_controller->get_toolbar_settings()->note_channel - 1;
    auto* project_manager = editor_controller->get_project_manager();
    const auto& colors = parent.colors().get_colors();

    const float frame_width = ImGui::GetWindowWidth();
    const float frame_padding = ImGui::GetStyle().WindowPadding.x;
    const float spacing = ImGui::GetStyle().ItemSpacing.x;

    ImGui::Separator();
    ImGui::Text("Channels");

    // we draw the swatches as an 8x2 grid, + the small number over it. when clicked, we would switch to that channel
    const float content_width = frame_width - frame_padding * 2.0f;
    const float total_spacing = spacing * static_cast<float>(SWATCHES_PER_ROW - 1);
    const float swatch_width = (content_width - total_spacing) / static_cast<float>(SWATCHES_PER_ROW);

    const ImVec2 swatch_size{ swatch_width, swatch_width };
    const ImU32 select_color = ImGui::GetColorU32(ImGui::GetStyle().Colors[ImGuiCol_TabSelected]);
    ImFont* font = ImGui::GetFont();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    for (size_t channel = 0; channel < 16; channel++) {
        bool is_channel_selected = active_channel == channel;

        if (channel % SWATCHES_PER_ROW != 0) ImGui::SameLine();
        ImVec2 cursor_pos = ImGui::GetCursorScreenPos();

        const ImVec2 swatch_min = cursor_pos;
        const ImVec2 swatch_max = cursor_pos + swatch_size;

        auto& color = colors[parent.colors().get_index((static_cast<size_t>(active_track) << 4) | channel)];

        // draw the swatch
        {
            if (is_channel_selected) {
                draw_list->AddRectFilled(
                    swatch_min - ImVec2{ 2.0f, 2.0f },
                    swatch_max + ImVec2{ 2.0f, 2.0f },
                    select_color,
                    3.0f
                );
            }

            auto color_u32 = IM_COL32(color[0] * 255.0f, color[1] * 255.0f, color[2] * 255.0f, 255);
            auto color_darker = IM_COL32(color[0] * 128.0f, color[1] * 128.0f, color[2] * 128.0f, 255);

            draw_list->AddRectFilled(
                swatch_min,
                swatch_max,
                ImGui::GetColorU32(color_u32, is_channel_selected ? 1.0f : 0.5f),
                1.0f
            );

            const std::string channel_str = std::to_string(channel + 1);
            const ImVec2 text_size = font->CalcTextSizeA(15.0f, FLT_MAX, 0.0f, channel_str.c_str());
            const ImVec2 text_pos = cursor_pos + (swatch_size - text_size) * 0.5f;

            draw_list->AddText(
                font,
                15.0f,
                text_pos,
                is_channel_selected ? color_darker : IM_COL32_BLACK,
                channel_str.c_str()
            );
        }

        if (ImGui::InvisibleButton(("##channelbtn" + std::to_string(channel)).c_str(), swatch_size)) {
            editor_controller->get_toolbar_settings()->note_channel = static_cast<int>(channel + 1);
        }
        ImGui::SetCursorScreenPos(cursor_pos);

        ImGui::Dummy(swatch_size);
    }

    ImGui::Separator();

    // the [+ Add Track] [- Remove Track] buttons
    const float button_width = frame_width * 0.5f - spacing;
    ImGui::SetNextItemWidth(button_width);
    if (ImGui::Button("New track")) {
        parent.controller().append_new_track();
    }
    ImGui::SameLine();

    ImGui::SetNextItemWidth(button_width);
    if (ImGui::Button("Delete track")) {
        parent.controller().remove_track(active_track);
    }
}

} // namespace

void draw_panel_track_list(MainWindow& parent) {
    auto* editor_controller = &parent.controller();

    static TrackListPopupState popup_state;

    ImGui::Text("Tracks");
    ImGui::Spacing();

#pragma region track list
    TrackListStyle style = get_track_list_style();
    draw_track_list_scroll_region(parent, popup_state, style);
    draw_track_controls(parent);
#pragma endregion
}

} // namespace andromeda::app
