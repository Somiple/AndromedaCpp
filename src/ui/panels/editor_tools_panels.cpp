#include "ui/panels/panels.h"

#define IMGUI_DEFINE_MATH_OPERATORS
#include <imgui.h>
#include <imgui_internal.h>

#include <array>
#include <format>
#include <string>
#include <utility>

#include "ui/app/editor_app.h"
#include "ui/app/main_window.h"
#include "ui/app/managers.h"
#include "ui/widgets/music_glyph.h"
#include "ui/widgets/toolbar_widgets.h"
#include "editor/editor_controller.h"
#include "editor/navigation.h"
#include "editor/project/project_manager.h"
#include "editor/settings/editor_settings.h"
#include "editor/timeline/timeline.h"
#include "core/util.h"

// ---------------------------------------------------------------------------
// Editor-tool panel group.
//
// Extracted from the former monolithic `app/ui/panels.cpp`. These panels all
// edit "what the pointer does" (active tool, snap grid, note defaults, onion
// skinning, colour indexing, zoom), so they evolve together and now live in
// one translation unit.
// ---------------------------------------------------------------------------

namespace andromeda::app {

using toolbar::icon_button;
using toolbar::number_field;
using toolbar::toolbar_separator;

void draw_panel_editor_tools_toollist(MainWindow& parent) {
    app::EditorToolSettings* editor_tool_settings = parent.controller().get_editor_tool_settings();
CENTERED({
    const ImageResources& images = parent.images();

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
    using editor::timeline::TimelineGridSnap;
    using editor::timeline::SnapModifier;
    using glyphs::choice;
    using glyphs::modifier_name;
    using glyphs::snap_name;

    auto* timeline = parent.controller().get_project_manager()->get_timeline();
    if (!timeline) return;

    const auto snap_get = [&] { return timeline->get_snap(); };
    const auto snap_set = [&](TimelineGridSnap v) { timeline->set_snap(v); };
    const auto mod_get = [&] { return timeline->get_snap_modifier(); };
    const auto mod_set = [&](SnapModifier v) { timeline->set_snap_modifier(v); };
    const auto snap_item = [&](const char* label, TimelineGridSnap v) { choice(label, v, snap_get, snap_set); };
    const auto mod_item = [&](const char* label, SnapModifier v) { choice(label, v, mod_get, mod_set); };

    const bool is_none = snap_get() == TimelineGridSnap::None;
    const bool mods_apply = !is_none && snap_get() != TimelineGridSnap::Bar;

CENTERED({
    const std::string label = is_none
        ? std::string("Off##note_snap")
        : std::format("{} {}##note_snap", snap_name(snap_get()), modifier_name(mod_get()));

    ImGui::Text("Snap:");
    ImGui::SameLine();
    if (ImGui::Button(label.c_str(), { 150.0, 0.0 })) ImGui::OpenPopup("##note_snap_popup");

    if (ImGui::BeginPopup("##note_snap_popup")) {
        snap_item("No snap", TimelineGridSnap::None);
        ImGui::Separator();
        snap_item("Bar", TimelineGridSnap::Bar);
        snap_item("Beat", TimelineGridSnap::Beat);
        snap_item("Half beat", TimelineGridSnap::HalfBeat);
        snap_item("Quarter beat", TimelineGridSnap::QuarterBeat);
        ImGui::Separator();
        snap_item("Whole note", TimelineGridSnap::WholeNote);
        snap_item("Half note", TimelineGridSnap::HalfNote);
        snap_item("Quarter note", TimelineGridSnap::QuarterNote);
        snap_item("Eighth note", TimelineGridSnap::EighthNote);
        snap_item("1/16th note", TimelineGridSnap::SixteenthNote);
        snap_item("1/32nd note", TimelineGridSnap::Note32);
        snap_item("1/64th note", TimelineGridSnap::Note64);
        snap_item("1/128th note", TimelineGridSnap::Note128);

        ImGui::Separator();
        ImGui::TextUnformatted("Modifiers");
        ImGui::BeginDisabled(!mods_apply);
        mod_item("Straight", SnapModifier::Straight);
        mod_item("Triplet (2/3)", SnapModifier::Triplet);
        mod_item("Dotted (3/2)", SnapModifier::Dotted);
        mod_item("Quintuplet (4/5)", SnapModifier::Quintuplet);
        mod_item("Septuplet (4/7)", SnapModifier::Septuplet);
        ImGui::EndDisabled();
        ImGui::EndPopup();
    }
});
}

void draw_panel_editor_tools_note_properties(MainWindow& parent) {
    app::ToolBarSettings* toolbar_settings = parent.controller().get_toolbar_settings();
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
    ViewSettings& vs = parent.settings();

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
    int curr_track = parent.controller().get_active_track();
    if (number_field("Curr. Track", &curr_track, 0, 65535, 50.0f)) {
        parent.controller().set_active_track(static_cast<std::uint16_t>(curr_track));
        parent.on_current_track_changed(curr_track);
    }

    toolbar_separator();

    ImGui::TextUnformatted("Color notes by");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(110.0f);
    if (ImGui::BeginCombo("##color_notes_by", to_string(parent.note_color_indexing()).c_str())) {
        const std::array<std::pair<NoteColorIndexing, const char*>, 3> opts = { {
            {NoteColorIndexing::Track, "Track"},
            {NoteColorIndexing::Channel, "Channel"},
            {NoteColorIndexing::ChannelTrack, "Track & Channel"},
        } };
        for (const auto& [value, label] : opts) {
            if (ImGui::Selectable(label, parent.note_color_indexing() == value)) {
                parent.set_note_color_indexing(value);
            }
        }
        ImGui::EndCombo();
    }
});
}

void draw_panel_editor_tools_zoom_controls(MainWindow& parent) {
CENTERED({
    const ImageResources & images = parent.images();
    if (icon_button("##zx_in", images.get_image_handle("zoom_x_in"), false, true, "X+", 0.0f,
        false)) {
        parent.curr_view_zoom_in_by(1.0f / editor::GLOBAL_ZOOM_FACTOR, 0.0f);
    }
    ImGui::SameLine();
    if (icon_button("##zx_out", images.get_image_handle("zoom_x_out"), false, true, "X-", 0.0f,
        false)) {
        parent.curr_view_zoom_in_by(editor::GLOBAL_ZOOM_FACTOR, 0.0f);
    }

    toolbar_separator();

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

} // namespace andromeda::app
