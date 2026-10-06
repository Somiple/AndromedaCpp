#include "app/ui/panels.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <cstdio>
#include <utility>

#include "app/main_window.h"
#include "editor/editing/track_editing.h"
#include "editor/navigation.h"
#include "editor/settings/editor_settings.h"
#include "util/util.h"

namespace andromeda::app {

namespace {

constexpr float TOOLBAR_ICON_SIZE = 20.0f;

void center_row_against(float row_height) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();

    const float half = std::max(0.0f, (row_height - ImGui::GetFrameHeight()) * 0.5f);
    w->DC.CurrLineTextBaseOffset = half + ImGui::GetStyle().FramePadding.y;
}

float toolbar_row_height() {
    return TOOLBAR_ICON_SIZE + ImGui::GetStyle().FramePadding.y * 2.0f;
}

constexpr float EGUI_CHECKBOX_SIZE = 14.0f;

constexpr float BAR_NUMBER_FONT_SIZE = 19.0f;

bool icon_button(const char* id, const ImageHandle& img, bool selected, bool enabled,
                 const char* fallback_label, float size = 0.0f, bool frame = true) {
    bool clicked = false;

    if (size <= 0.0f) {
        size = img.valid() ? img.width : 20.0f;
    }

    if (!frame) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    }

    if (img.valid()) {
        const ImVec4 tint = enabled ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                    : ImVec4(1.0f, 1.0f, 1.0f, 0.45f);

        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.0f, 0.361f, 0.502f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.0f, 0.42f, 0.58f, 1.0f));
        }

        clicked = ImGui::ImageButton(id, static_cast<ImTextureID>(img.texture_id),
                                     ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                                     ImVec4(0, 0, 0, 0), tint);

        if (selected) {
            ImGui::PopStyleColor(2);
        }

        if (!enabled) {
            clicked = false;
        }
    } else {
        ImGui::BeginDisabled(!enabled);
        clicked = ImGui::Button(fallback_label);
        ImGui::EndDisabled();
    }

    if (!frame) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    return clicked;
}

bool egui_checkbox(const char* id, bool* value) {
    const float sz = EGUI_CHECKBOX_SIZE;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(sz, sz));
    const bool pressed = ImGui::IsItemClicked();
    if (pressed) {
        *value = !*value;
    }

    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImU32 bg = hovered ? IM_COL32(70, 70, 70, 255) : IM_COL32(60, 60, 60, 255);
    draw->AddRectFilled(p, {p.x + sz, p.y + sz}, bg, 2.0f);
    draw->AddRect(p, {p.x + sz, p.y + sz}, IM_COL32(90, 90, 90, 255), 2.0f);

    if (*value) {
        const float pad = sz * 0.28f;
        const ImVec2 a{p.x + pad, p.y + sz * 0.52f};
        const ImVec2 b{p.x + sz * 0.42f, p.y + sz - pad};
        const ImVec2 c{p.x + sz - pad, p.y + pad};
        draw->AddLine(a, b, IM_COL32(220, 220, 220, 255), 1.4f);
        draw->AddLine(b, c, IM_COL32(220, 220, 220, 255), 1.4f);
    }

    return pressed;
}

bool egui_slider_int(const char* id, int* value, int v_min, int v_max, float width) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    const int old_value = *value;

    if (active && v_max > v_min) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / std::max(width, 1.0f), 0.0f,
                                   1.0f);
        *value = v_min + static_cast<int>(t * static_cast<float>(v_max - v_min) + 0.5f);
    }

    const float rail_radius = std::max(h * 0.25f, 2.0f);
    const float cy = p.y + h * 0.5f;
    const float r_pad = h / 2.5f + 2.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect({p.x - r_pad, cy - r_pad}, {p.x + width + r_pad, cy + r_pad}, false);

    dl->AddRectFilled({p.x, cy - rail_radius}, {p.x + width, cy + rail_radius},
                      IM_COL32(60, 60, 60, 255), 2.0f);

    const float frac =
        v_max > v_min ? std::clamp(static_cast<float>(*value - v_min) /
                                       static_cast<float>(v_max - v_min),
                                   0.0f, 1.0f)
                      : 0.0f;

    const float r = h / 2.5f;
    const ImVec2 c{p.x + frac * width, cy};
    dl->AddCircleFilled(c, r,
                        (hovered || active) ? IM_COL32(230, 230, 230, 255)
                                            : IM_COL32(200, 200, 200, 255));
    dl->AddCircle(c, r, IM_COL32(120, 120, 120, 255), 0, 1.0f);

    dl->PopClipRect();

    return *value != old_value;
}

void vertical_separator() {
    ImGui::SameLine();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine({p.x + 3.0f, p.y}, {p.x + 3.0f, p.y + h},
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(7.0f, h));
    ImGui::SameLine();
}

bool number_field(const char* label, int* value, int min_value, int max_value, float width) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(width);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", *value);

    ImGui::PushID(label);
    const bool changed = ImGui::InputText("##v", buf, sizeof(buf),
                                          ImGuiInputTextFlags_CharsDecimal |
                                              ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopID();

    if (changed) {
        int parsed = *value;
        if (std::sscanf(buf, "%d", &parsed) == 1) {
            *value = std::clamp(parsed, min_value, max_value);
        }
    }

    return changed;
}

#define CENTERED(...) \
    do { \
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f)); \
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f)); \
        center_row_against(toolbar_row_height()); \
        { __VA_ARGS__ } \
        ImGui::PopStyleVar(2); \
    } while (0)
}

void draw_panel_editor_tools_toollist(MainWindow& parent) {
    app::EditorToolSettings* editor_tool_settings = parent.editor_controller.get_editor_tool_settings();
CENTERED({
    const ImageResources& images = parent.image_resources;
    
    if (icon_button("##pencil", images.get_image_handle("pencil"),
        editor_tool_settings->curr_tool == EditorTool::Pencil, true, "Pencil")) {
        editor_tool_settings->curr_tool = EditorTool::Pencil;
    }
    ImGui::SameLine();
    if (icon_button("##eraser", images.get_image_handle("eraser"),
        editor_tool_settings->curr_tool == EditorTool::Eraser, true, "Eraser")) {
        editor_tool_settings->curr_tool = EditorTool::Eraser;
    }
    ImGui::SameLine();
    if (icon_button("##select", images.get_image_handle("select"),
        editor_tool_settings->curr_tool == EditorTool::Selector, true, "Select")) {
        editor_tool_settings->curr_tool = EditorTool::Selector;
    }
});
}

void draw_panel_editor_tools_note_snap(MainWindow& parent) {
    app::EditorToolSettings* editor_tool_settings = parent.editor_controller.get_editor_tool_settings();
CENTERED({
    if (ImGui::Button("Note Snap")) {
        ImGui::OpenPopup("##note_snap_popup");
    }

    if (ImGui::BeginPopup("##note_snap_popup")) {
        for (const auto& [ratio, name] : editor::SNAP_MAPPINGS) {
            bool selected = ratio == editor_tool_settings->snap_ratio;
            if (ImGui::Checkbox(name, &selected)) {
                editor_tool_settings->snap_ratio = ratio;
            }
        }
        ImGui::EndPopup();
    }
});
}

void draw_panel_editor_tools_note_properties(MainWindow& parent) {
    app::ToolBarSettings* toolbar_settings = parent.editor_controller.get_toolbar_settings();
CENTERED({
    number_field("Gate", &toolbar_settings->note_gate, 1, 65535, 30.0f);
    ImGui::SameLine(); 
    number_field("Velo", &toolbar_settings->note_velocity, 1, 127, 30.0f);
    ImGui::SameLine();
    number_field("Chan", &toolbar_settings->note_channel, 1, 16, 30.0f);
});
}

void draw_panel_editor_tools_track_options(MainWindow& parent) {
CENTERED({
    // read without the lock: only safe because panels and gl pass share the ui thread
    ViewSettings& vs = parent.view_settings->value;

    ImGui::TextUnformatted("View Track");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::BeginCombo("##onion_track", to_string(vs.pr_onion_state).c_str())) {
        const std::array<std::pair<VS_PianoRoll_OnionState, const char*>, 4> opts = { {
            {VS_PianoRoll_OnionState::NoOnion, "No tracks"},
            {VS_PianoRoll_OnionState::ViewAll, "All tracks"},
            {VS_PianoRoll_OnionState::ViewNext, "Next track"},
            {VS_PianoRoll_OnionState::ViewPrevious, "Previous track"},
        } };
        for (const auto& [value, label] : opts) {
            if (ImGui::Selectable(label, vs.pr_onion_state == value)) {
                vs.pr_onion_state = value;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("Onion Color");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##onion_coloring", to_string(vs.pr_onion_coloring).c_str())) {
        const std::array<std::pair<VS_PianoRoll_OnionColoring, const char*>, 3> opts = { {
            {VS_PianoRoll_OnionColoring::GrayedOut, "Grayed Out"},
            {VS_PianoRoll_OnionColoring::PartialColor, "Partial Color"},
            {VS_PianoRoll_OnionColoring::FullColor, "Full Color"},
        } };
        for (const auto& [value, label] : opts) {
            if (ImGui::Selectable(label, vs.pr_onion_coloring == value)) {
                vs.pr_onion_coloring = value;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    int curr_track = parent.editor_controller.get_active_track();
    if (number_field("Curr. Track", &curr_track, 0, 65535, 50.0f)) {
        parent.editor_controller.set_active_track(static_cast<std::uint16_t>(curr_track));
        parent.on_current_track_changed(curr_track);
    }

    vertical_separator();

    ImGui::TextUnformatted("Color notes by");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##color_notes_by", to_string(parent.note_color_indexing).c_str())) {
        const std::array<std::pair<NoteColorIndexing, const char*>, 3> opts = { {
            {NoteColorIndexing::Track, "Track"},
            {NoteColorIndexing::Channel, "Channel"},
            {NoteColorIndexing::ChannelTrack, "Track & Channel"},
        } };
        for (const auto& [value, label] : opts) {
            if (ImGui::Selectable(label, parent.note_color_indexing == value)) {
                parent.note_color_indexing = value;
            }
        }
        ImGui::EndCombo();
    }
});
}

void draw_panel_editor_tools_zoom_controls(MainWindow& parent) {
CENTERED({
    const ImageResources & images = parent.image_resources;
    if (icon_button("##zx_in", images.get_image_handle("zoom_x_in"), false, true, "X+", 0.0f,
        false)) {
        parent.curr_view_zoom_in_by(1.0f / editor::GLOBAL_ZOOM_FACTOR, 0.0f);
    }
    ImGui::SameLine();
    if (icon_button("##zx_out", images.get_image_handle("zoom_x_out"), false, true, "X-", 0.0f,
        false)) {
        parent.curr_view_zoom_in_by(editor::GLOBAL_ZOOM_FACTOR, 0.0f);
    }

    vertical_separator();

    if (icon_button("##zy_in", images.get_image_handle("zoom_y_in"), false, true, "Y+", 0.0f,
        false)) {
        parent.curr_view_zoom_in_by(0.0f, 1.0f / editor::GLOBAL_ZOOM_FACTOR);
    }
    ImGui::SameLine();
    if (icon_button("##zy_out", images.get_image_handle("zoom_y_out"), false, true, "Y-", 0.0f,
        false)) {
        parent.curr_view_zoom_in_by(0.0f, editor::GLOBAL_ZOOM_FACTOR);
    }
});
}

/*
void draw_panel_editor_tools(MainWindow& parent) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));

    center_row_against(toolbar_row_height());

    const ImageResources& images = parent.image_resources;

    if (icon_button("##pencil", images.get_image_handle("pencil"),
                    parent.editor_tool_settings->curr_tool == EditorTool::Pencil, true, "Pencil")) {
        parent.editor_tool_settings->curr_tool = EditorTool::Pencil;
    }
    ImGui::SameLine();
    if (icon_button("##eraser", images.get_image_handle("eraser"),
                    parent.editor_tool_settings->curr_tool == EditorTool::Eraser, true, "Eraser")) {
        parent.editor_tool_settings->curr_tool = EditorTool::Eraser;
    }
    ImGui::SameLine();
    if (icon_button("##select", images.get_image_handle("select"),
                    parent.editor_tool_settings->curr_tool == EditorTool::Selector, true, "Select")) {
        parent.editor_tool_settings->curr_tool = EditorTool::Selector;
    }

    vertical_separator();

    if (ImGui::Button("Note Snap")) {
        ImGui::OpenPopup("##note_snap_popup");
    }
    if (ImGui::BeginPopup("##note_snap_popup")) {
        for (const auto& [ratio, name] : editor::SNAP_MAPPINGS) {
            bool selected = ratio == parent.editor_tool_settings->snap_ratio;
            if (ImGui::Checkbox(name, &selected)) {
                parent.editor_tool_settings->snap_ratio = ratio;
            }
        }
        ImGui::EndPopup();
    }

    vertical_separator();

    number_field("Gate", &parent.toolbar_settings->note_gate, 1, 65535, 30.0f);
    ImGui::SameLine();
    number_field("Velo", &parent.toolbar_settings->note_velocity, 1, 127, 30.0f);
    ImGui::SameLine();
    number_field("Chan", &parent.toolbar_settings->note_channel, 1, 16, 30.0f);

    vertical_separator();

    // read without the lock: only safe because panels and gl pass share the ui thread
    ViewSettings& vs = parent.view_settings->value;

    ImGui::TextUnformatted("View Track");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100.0f);
    if (ImGui::BeginCombo("##onion_track", to_string(vs.pr_onion_state).c_str())) {
        const std::array<std::pair<VS_PianoRoll_OnionState, const char*>, 4> opts = {{
            {VS_PianoRoll_OnionState::NoOnion, "No tracks"},
            {VS_PianoRoll_OnionState::ViewAll, "All tracks"},
            {VS_PianoRoll_OnionState::ViewNext, "Next track"},
            {VS_PianoRoll_OnionState::ViewPrevious, "Previous track"},
        }};
        for (const auto& [value, label] : opts) {
            if (ImGui::Selectable(label, vs.pr_onion_state == value)) {
                vs.pr_onion_state = value;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    ImGui::TextUnformatted("Onion Color");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##onion_coloring", to_string(vs.pr_onion_coloring).c_str())) {
        const std::array<std::pair<VS_PianoRoll_OnionColoring, const char*>, 3> opts = {{
            {VS_PianoRoll_OnionColoring::GrayedOut, "Grayed Out"},
            {VS_PianoRoll_OnionColoring::PartialColor, "Partial Color"},
            {VS_PianoRoll_OnionColoring::FullColor, "Full Color"},
        }};
        for (const auto& [value, label] : opts) {
            if (ImGui::Selectable(label, vs.pr_onion_coloring == value)) {
                vs.pr_onion_coloring = value;
            }
        }
        ImGui::EndCombo();
    }

    ImGui::SameLine();
    int curr_track = vs.pr_curr_track;
    if (number_field("Curr. Track", &curr_track, 0, 65535, 50.0f)) {
        vs.pr_curr_track = static_cast<std::uint16_t>(curr_track);
        parent.on_current_track_changed(vs.pr_curr_track);
    }

    vertical_separator();

    ImGui::TextUnformatted("Color notes by");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##color_notes_by", to_string(parent.note_color_indexing).c_str())) {
        const std::array<std::pair<NoteColorIndexing, const char*>, 3> opts = {{
            {NoteColorIndexing::Track, "Track"},
            {NoteColorIndexing::Channel, "Channel"},
            {NoteColorIndexing::ChannelTrack, "Track & Channel"},
        }};
        for (const auto& [value, label] : opts) {
            if (ImGui::Selectable(label, parent.note_color_indexing == value)) {
                parent.note_color_indexing = value;
            }
        }
        ImGui::EndCombo();
    }

    vertical_separator();

    if (icon_button("##zx_in", images.get_image_handle("zoom_x_in"), false, true, "X+", 0.0f,
                    false)) {
        parent.curr_view_zoom_in_by(1.0f / editor::GLOBAL_ZOOM_FACTOR, 0.0f);
    }
    ImGui::SameLine();
    if (icon_button("##zx_out", images.get_image_handle("zoom_x_out"), false, true, "X-", 0.0f,
                    false)) {
        parent.curr_view_zoom_in_by(editor::GLOBAL_ZOOM_FACTOR, 0.0f);
    }

    vertical_separator();

    if (icon_button("##zy_in", images.get_image_handle("zoom_y_in"), false, true, "Y+", 0.0f,
                    false)) {
        parent.curr_view_zoom_in_by(0.0f, 1.0f / editor::GLOBAL_ZOOM_FACTOR);
    }
    ImGui::SameLine();
    if (icon_button("##zy_out", images.get_image_handle("zoom_y_out"), false, true, "Y-", 0.0f,
                    false)) {
        parent.curr_view_zoom_in_by(0.0f, editor::GLOBAL_ZOOM_FACTOR);
    }

    ImGui::PopStyleVar(2);
}*/

void draw_panel_playback_buttons(MainWindow& parent) {
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(4.0f, 4.0f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(1.0f, 1.0f));

    center_row_against(EGUI_CHECKBOX_SIZE);

    const editor::MIDITick ppq = parent.get_ppq();
    auto& engine = *parent.audio_engine;

    const auto navigate_keeping_play_state = [&parent, &engine](editor::MIDITick ticks) {
        const bool last_play_state = engine.is_playing();
        if (last_play_state) {
            engine.toggle_playback();
        }
        // fixed rust bug: moved the engine but not the drawn playhead, so the two disagreed
        parent.playhead->set_start(ticks);
        if (last_play_state) {
            parent.toggle_playback_from_start();
        } else {
            parent.show_tick(ticks);
        }
    };

    if (ImGui::Button("<-")) {
        const editor::MIDITick t = engine.get_playback_ticks();
        navigate_keeping_play_state(t > ppq ? t - ppq : 0);
    }
    ImGui::SameLine();
    if (ImGui::Button(engine.is_playing() ? "||" : "|>")) {
        parent.toggle_playback_from_start();
    }
    ImGui::SameLine();
    if (ImGui::Button("->")) {
        navigate_keeping_play_state(engine.get_playback_ticks() + ppq);
    }

    vertical_separator();

    ImGui::TextUnformatted("Autoscroll");
    ImGui::SameLine();

    egui_checkbox("##autoscroll", &parent.view_settings->value.pr_autoscroll);

    ImGui::PopStyleVar(2);
}

namespace {

void draw_chip(ImDrawList* dl, ImVec2 pos, ImVec2 size, ImU32 color, float alpha_mul=1.0f) {

    dl->AddRectFilled(
        pos,
        pos + size,
        ImGui::GetColorU32(color, alpha_mul),
        6.0f
    );
}

}

void draw_panel_fancy_playback(MainWindow& parent)
{
    // TODO: mutex
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
    andromeda::editor::ProjectManager* project = parent.editor_controller.get_project_manager();
    andromeda::editor::TempoMap* tempo_map = project->get_tempo_map();

    editor::MIDITick ticks = parent.get_playback_manager()->get_playback_ticks();
    float secs = tempo_map->ticks_to_secs_from_map(project->get_ppq(), ticks);

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
            // const char* time_text_tmp = "00:00:00";
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
            const char* measure_beat_tmp = "06:28";
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
            text_size = font->CalcTextSizeA(font_size, FLT_MAX, 0.0f, measure_beat_tmp);
            text_pos = {
                start_x,
                inner_pos.y + inner_size.y - text_size.y
            };

            draw_list->AddText(
                font,
                font_size,
                text_pos,
                text_color,
                measure_beat_tmp
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
            std::string bpm_str = std::to_string(std::llroundf(tempo_map->get_bpm_at_tick(ticks)));
            const char* bpm_cstr = bpm_str.c_str();

            // float text_height = font->CalcTextSizeA(chip_text_size, FLT_MAX, 0.0f, "#").y;

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

    // playback buttons
    {

    }
}

void draw_panel_side_controls(MainWindow& parent) {
    const ImageResources& images = parent.image_resources;
    editor::EditorController* controller = &parent.editor_controller;

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
    // fixed: the knob now uses the playhead line's view and position, so zoom moves both alike
    const auto [min_tick, max_tick] = parent.get_view_tick_range_with_playback();

    // span the roll's note area exactly, as the bar numbers do, not the padded content region
    const float kb_width = parent.get_keyboard_width();
    const float slider_x = ImGui::GetWindowPos().x + kb_width;
    const float slider_w = std::max(1.0f, ImGui::GetWindowSize().x - kb_width);
    ImGui::SetCursorScreenPos({slider_x, ImGui::GetCursorScreenPos().y});

    int playhead_time = static_cast<int>(parent.get_playhead_pos(false));

    if (egui_slider_int("##playhead", &playhead_time, static_cast<int>(min_tick),
                        static_cast<int>(max_tick), slider_w)) {
        const auto snap_ratio = parent.editor_controller.get_editor_tool_settings()->snap_ratio;
        const editor::MIDITick ppq = parent.get_ppq();

        editor::MIDITick min_snap_length = 1;
        if (snap_ratio.first != 0 && snap_ratio.second != 0) {
            min_snap_length = (ppq * 4 * snap_ratio.first) / snap_ratio.second;
            if (min_snap_length == 0) {
                min_snap_length = 1;
            }
        }

        const editor::MIDITick t = static_cast<editor::MIDITick>(std::max(0, playhead_time));
        const editor::MIDITick snapped =
            ((t + min_snap_length / 2) / min_snap_length) * min_snap_length;

        // a seek while autoscrolling moves the start the view is measured from; hold the
        // view where it is on screen so only the playhead moves
        if (parent.audio_engine->is_playing() && parent.view_settings->value.pr_autoscroll) {
            const float view_min = static_cast<float>(min_tick);
            if (parent.render_type == RenderType::TrackView) {
                std::unique_lock lock(parent.track_nav->mutex);
                parent.track_nav->value.tick_pos = view_min;
                parent.track_nav->value.tick_pos_smoothed = view_min;
            } else {
                std::unique_lock lock(parent.nav->mutex);
                parent.nav->value.tick_pos = view_min;
                parent.nav->value.tick_pos_smoothed = view_min;
            }
        }

        parent.playhead->set_start(snapped);

        if (parent.render_type == RenderType::TrackView) {
            std::unique_lock lock(parent.nav->mutex);
            parent.nav->value.tick_pos =
                static_cast<float>(snapped > 960 ? snapped - 960 : 0);
        }
    }
}

void draw_panel_scroll_navigation(MainWindow& parent) {
    editor::MIDITick last_start = 0;
    {
        std::vector<midi::MIDITrack>& tracks = *parent.editor_controller.get_project_manager()->get_tracks();
        for (midi::MIDITrack& track : tracks) {
            if (!track.get_notes().empty()) {
                last_start = std::max(last_start, track.get_notes().back().start);
            }
        }
    }

    const float tail = static_cast<float>(std::max<std::uint16_t>(parent.get_ppq(), 1)) * 4.0f * 8.0f;
    const float latest = static_cast<float>(last_start) + tail;

    const bool is_track = parent.render_type == RenderType::TrackView;

    float tick_pos = 0.0f;
    float tick_end = 0.0f;
    if (is_track) {
        std::shared_lock lock(parent.track_nav->mutex);
        tick_pos = parent.track_nav->value.tick_pos;
        tick_end = tick_pos + parent.track_nav->value.zoom_ticks;
    } else {
        std::shared_lock lock(parent.nav->mutex);
        tick_pos = parent.nav->value.tick_pos;
        tick_end = tick_pos + parent.nav->value.zoom_ticks;
    }

    float scrolled = 0.0f;
    bool following = false;
    if (parent.audio_engine->is_playing()) {
        {
            std::lock_guard lock(parent.view_settings->mutex);
            following = parent.view_settings->value.pr_autoscroll;
        }

        if (following) {
            const editor::MIDITick now = parent.audio_engine->get_playback_ticks();
            const editor::MIDITick from = parent.audio_engine->get_playback_start_tick();
            scrolled = static_cast<float>(now > from ? now - from : 0);
            tick_pos += scrolled;
            tick_end += scrolled;
        }
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetFrameHeight() * 0.55f;
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const float track_y = p0.y + h * 0.5f;
    const float range = std::max(latest, 1.0f);

    constexpr float RAIL_HALF = 4.0f;
    constexpr float THUMB_HALF = 5.0f;
    constexpr float MIN_THUMB = 22.0f;

    draw->AddRectFilled({p0.x, track_y - RAIL_HALF}, {p0.x + w, track_y + RAIL_HALF},
                        IM_COL32(16, 16, 18, 255), RAIL_HALF);

    float x0 = p0.x + (tick_pos / range) * w;
    float x1 = p0.x + (std::min(tick_end, range) / range) * w;
    if (x1 - x0 < MIN_THUMB) {
        const float centre = (x0 + x1) * 0.5f;
        x0 = centre - MIN_THUMB * 0.5f;
        x1 = centre + MIN_THUMB * 0.5f;
    }
    x0 = std::max(x0, p0.x);
    x1 = std::min(x1, p0.x + w);

    ImGui::InvisibleButton("##scroll_nav", ImVec2(w, ImGui::GetFrameHeight()));

    const auto set_view_pos = [&](float pos) {
        if (is_track) {
            std::unique_lock lock(parent.track_nav->mutex);
            parent.track_nav->value.tick_pos = pos;
        } else {
            std::unique_lock lock(parent.nav->mutex);
            parent.nav->value.tick_pos = pos;
        }
    };

    // while playing, dragging looks elsewhere for as long as the button is held; letting go
    // brings the view back to the playhead where it was
    if (ImGui::IsItemActivated()) {
        parent.scroll_return_pos.reset();
        if (following) {
            parent.scroll_return_pos = tick_pos - scrolled;
        }
    }
    if (ImGui::IsItemDeactivated()) {
        if (parent.scroll_return_pos && following) {
            set_view_pos(*parent.scroll_return_pos);
        }
        parent.scroll_return_pos.reset();
    }

    const bool active = ImGui::IsItemActive();
    const ImU32 thumb = active            ? IM_COL32(122, 122, 130, 255)
                        : ImGui::IsItemHovered() ? IM_COL32(96, 96, 103, 255)
                                                 : IM_COL32(72, 72, 78, 255);
    draw->AddRectFilled({x0, track_y - THUMB_HALF}, {x1, track_y + THUMB_HALF}, thumb,
                        THUMB_HALF);
    if (active) {
        const float mx = ImGui::GetIO().MousePos.x - p0.x;
        const float span = std::max(1.0f, tick_end - tick_pos);
        const float new_pos = std::max(0.0f, (mx / std::max(w, 1.0f)) * range - span * 0.5f);

        // while following playback this is an offset from the playhead and may go below 0;
        // clamping it kept the view from going back before the ground already played
        set_view_pos(following ? new_pos - scrolled : std::max(0.0f, new_pos - scrolled));
    }
}

void draw_panel_scroll_navigation_vertical(MainWindow& parent) {
    // TODO: have the controller report the used track count instead
    const float range = static_cast<float>(parent.editor_controller.get_track_editing()->get_used_track_count()) + 10.0f;

    float track_pos = 0.0f;
    float zoom_tracks = 0.0f;
    {
        std::shared_lock lock(parent.track_nav->mutex);
        track_pos = parent.track_nav->value.track_pos;
        zoom_tracks = parent.track_nav->value.zoom_tracks;
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const float h = ImGui::GetContentRegionAvail().y;
    if (w <= 0.0f || h <= 0.0f) {
        return;
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();

    constexpr float RAIL_HALF = 4.0f;
    constexpr float THUMB_HALF = 5.0f;
    constexpr float MIN_THUMB = 22.0f;

    const float cx = p0.x + w * 0.5f;
    draw->AddRectFilled({cx - RAIL_HALF, p0.y}, {cx + RAIL_HALF, p0.y + h},
                        IM_COL32(16, 16, 18, 255), RAIL_HALF);

    float y0 = p0.y + (std::clamp(track_pos, 0.0f, range) / range) * h;
    float y1 = p0.y + (std::clamp(track_pos + zoom_tracks, 0.0f, range) / range) * h;
    if (y1 - y0 < MIN_THUMB) {
        const float centre = (y0 + y1) * 0.5f;
        y0 = centre - MIN_THUMB * 0.5f;
        y1 = centre + MIN_THUMB * 0.5f;
    }
    if (y0 < p0.y) {
        y1 += p0.y - y0;
        y0 = p0.y;
    }
    if (y1 > p0.y + h) {
        y0 -= y1 - (p0.y + h);
        y1 = p0.y + h;
    }

    ImGui::InvisibleButton("##track_scroll", ImVec2(w, h));

    const bool active = ImGui::IsItemActive();
    const ImU32 thumb = active                   ? IM_COL32(122, 122, 130, 255)
                        : ImGui::IsItemHovered() ? IM_COL32(96, 96, 103, 255)
                                                 : IM_COL32(72, 72, 78, 255);
    draw->AddRectFilled({cx - THUMB_HALF, y0}, {cx + THUMB_HALF, y1}, thumb, THUMB_HALF);

    if (active) {
        const float my = ImGui::GetIO().MousePos.y - p0.y;
        const float new_pos =
            std::clamp((my / h) * range - zoom_tracks * 0.5f, 0.0f, range);

        std::unique_lock lock(parent.track_nav->mutex);
        parent.track_nav->value.track_pos = new_pos;
    }
}

void draw_panel_data_viewer(MainWindow& parent) {
    // read without the lock: only safe because panels and gl pass share the ui thread
    ViewSettings& vs = parent.view_settings->value;

    // using new dockable panel system!

    const ImGuiStyle& style = ImGui::GetStyle();
    const ImVec2 origin = ImGui::GetCursorScreenPos();
    const ImVec2 wp = ImGui::GetWindowPos();
    const ImVec2 ws = ImGui::GetWindowSize();
    const ImVec2 wmax{ wp.x + ws.x, wp.y + ws.y };

    const float header_h = ImGui::GetFrameHeight() + style.ItemSpacing.y;
    const ImVec2 hole_min{ origin.x + parent.get_keyboard_width(), origin.y + header_h };
    const ImVec2 content_max = ImGui::GetCurrentWindow()->ContentRegionRect.Max;
    const ImVec2 avail{ std::max(1.0f, content_max.x - hole_min.x),
                       std::max(1.0f, content_max.y - hole_min.y) };

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 fill = ImGui::GetColorU32(ImGuiCol_WindowBg);
    dl->AddRectFilled(wp, { wmax.x, hole_min.y }, fill);
    dl->AddRectFilled({ wp.x, hole_min.y }, { hole_min.x, wmax.y }, fill);
    if (hole_min.x + avail.x < wmax.x) {
        dl->AddRectFilled({ hole_min.x + avail.x, hole_min.y }, wmax, fill);
    }
    if (hole_min.y + avail.y < wmax.y) {
        dl->AddRectFilled({ hole_min.x, hole_min.y + avail.y }, { hole_min.x + avail.x, wmax.y },
            fill);
    }

    ImGui::SetCursorScreenPos({ hole_min.x, origin.y });
    ImGui::TextUnformatted("Property");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120.0f);
    if (ImGui::BeginCombo("##property", to_string(vs.pr_dataview_state).c_str())) {
        if (ImGui::Selectable("Velocity",
            vs.pr_dataview_state == VS_PianoRoll_DataViewState::NoteVelocities)) {
            vs.pr_dataview_state = VS_PianoRoll_DataViewState::NoteVelocities;
        }
        if (ImGui::Selectable("Pitch Bend",
            vs.pr_dataview_state == VS_PianoRoll_DataViewState::PitchBend)) {
            vs.pr_dataview_state = VS_PianoRoll_DataViewState::PitchBend;
        }
        ImGui::EndCombo();
    }

    parent.data_view_rect = editor::ViewRect{ hole_min.x, hole_min.y, avail.x, avail.y };
    dl->AddCallback([](const ImDrawList*, const ImDrawCmd* cmd) {
        static_cast<MainWindow*>(cmd->UserCallbackData)->render_data_view_pass();
        }, &parent);
    dl->AddCallback(ImDrawCallback_ResetRenderState, nullptr);

    ImGui::SetCursorScreenPos(hole_min);
    ImGui::Dummy(avail);

    parent.handle_data_view_inputs(parent.data_view_rect, ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup));

    editor::DataEditing* data_editing = parent.editor_controller.get_data_editing();
    if (data_editing != nullptr &&
        data_editing->get_flag(editor::data_edit_flags::DATA_EDIT_DRAW_EDIT_LINE)) {
        const auto [pt1, pt2] = data_editing->get_data_view_line_points();
        dl->AddLine({ pt1.x, pt1.y }, { pt2.x, pt2.y },
            IM_COL32(255, 255, 255, 255), 1.0f);
    }
}

void draw_panel_bar_numbers(MainWindow& parent) {
    const auto [min_tick, max_tick] = parent.get_view_tick_range_with_playback();
    const float zoom_ticks = static_cast<float>(max_tick - min_tick);
    if (zoom_ticks <= 0.0f) {
        return;
    }

    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float avail_w = ImGui::GetContentRegionAvail().x;

    const float origin = ImGui::GetWindowPos().x;
    const float full_w = ImGui::GetWindowSize().x;
    const float space_alloc = parent.get_keyboard_width();
    const float roll_w = std::max(0.0f, full_w - space_alloc);
    const float label_gap = ImGui::GetStyle().WindowPadding.x;

    ImDrawList* draw = ImGui::GetWindowDrawList();
    draw->PushClipRect({origin, p0.y}, {origin + full_w, p0.y + BAR_NUMBER_FONT_SIZE * 2.0f},
                       false);

    std::size_t bar_num = 0;
    editor::MIDITick curr_bar_tick = 0;

    while (curr_bar_tick < max_tick) {
        const auto [bar_tick, bar_length] = parent.bar_cacher->get_bar_interval(bar_num);
        if (bar_length == 0) {
            break;
        }

        if ((bar_tick + bar_length) < min_tick) {
            curr_bar_tick += bar_length;
            bar_num += 1;
            continue;
        }

        const float ui_pos =
            ((static_cast<float>(curr_bar_tick > min_tick ? curr_bar_tick - min_tick : 0)) /
             zoom_ticks) * roll_w + space_alloc + label_gap;

        char label[16];
        std::snprintf(label, sizeof(label), "%zu", bar_num + 1);
        draw->AddText(ImGui::GetFont(), BAR_NUMBER_FONT_SIZE, {origin + ui_pos, p0.y},
                      IM_COL32(160, 160, 160, 255), label);

        curr_bar_tick += bar_length;
        bar_num += 1;

        if (bar_num > 4096) {
            break;
        }
    }

    draw->PopClipRect();

    ImGui::Dummy(ImVec2(avail_w, BAR_NUMBER_FONT_SIZE));
}

void draw_panel_process_stats(MainWindow& parent) {
    parent.sys_stats.update();

    const float cpu = parent.sys_stats.cpu_usage;
    const float ram_pers = parent.sys_stats.memory_pers;

    const auto colour_for = [](float pers) {
        if (pers >= 90.0f) {
            return ImVec4(1.0f, 0.0f, 0.0f, 1.0f);
        }
        if (pers >= 50.0f) {
            return ImVec4(1.0f, 1.0f, 0.0f, 1.0f);
        }
        return ImGui::GetStyleColorVec4(ImGuiCol_Text);
    };

    ImGui::TextColored(colour_for(cpu), "CPU: %.2f%%", cpu);
    vertical_separator();
    ImGui::TextColored(colour_for(ram_pers), "RAM: %s (%.1f%%)",
                       parent.sys_stats.memory_usage.to_string().c_str(), ram_pers);
}

}
