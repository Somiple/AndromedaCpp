// REFACTOR: dock-space wiring extracted verbatim from main_window.cpp (lines 289-460).
// main_window.cpp keeps only window-core responsibilities (construction, accessor block,
// panel orchestration). Behaviour unchanged.
#include "ui/app/main_window.h"

#include <glad/glad.h>
// glad must be included before glfw
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <format>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <tuple>

#include <stb_image_write.h>

#include "editor_app.h"
#include "managers.h"

#include "ui/rendering/data_view.h"
#include "ui/rendering/note_gpu_cache.h"
#include "ui/rendering/note_occlusion.h"
#include "ui/rendering/note_uploader.h"
#include "ui/rendering/piano_roll.h"
#include "ui/rendering/screen_coverage.h"
#include "ui/style/theme.h"
#include "ui/dialogs/crash_dialog.h"
#include "ui/dialogs/filter_channels.h"
#include "ui/dialogs/simple_dialog.h"
#include "ui/dialogs/editor_info.h"
#include "ui/dialogs/main_menu_bar.h"
#include "ui/dialogs/manual.h"
#include "ui/panels/panels.h"
#include "ui/layout/dock_panel.h"
#include "ui/widgets/data_view_widget.h"
#include "ui/widgets/piano_roll_widget.h"
#include "ui/widgets/track_view_widget.h"
#include "editor/plugins/plugin_andromeda_obj.h"
#include "editor/plugins/plugin_dialog.h"
#include "editor/plugins/plugin_error_dialog.h"
#include "editor/settings/editor_settings.h"
#include "editor/settings/project_settings.h"
#include "editor/editor_controller.h"
#include "editor/util.h"
#include "midi/midi_file.h"
#include "core/crash_handler.h"
#include "core/debugger.h"

#include "main_window_internal.h"
namespace andromeda::app {

// REFACTOR: row-height constants now live in main_window_internal.h so that this TU and
// main_window.cpp share one definition instead of duplicating the anonymous namespace.
using internal::TOOLBAR_ROW_H;
using internal::BAR_NUMBER_ROW_H;
void MainWindow::init_dock_panels() {
    const auto px = [this](float v) { return v * app_scale_; };

    auto& menu_group = dock_manager->add_group("menu_bar_group", {
        .position = DockPosition::Top,
        .fixed = true,
        .resizable = false,
        .extent_fn = [] { return ImGui::GetFrameHeight(); },
        .window_flags = ImGuiWindowFlags_MenuBar
        });

    menu_group.add_panel<FnDock>("menu_bar", "Menu bar",
        DockPanelConfig{ .is_flex = true, .padding = { 0.0f, 0.0f } },
        [this] {
            if (menu_bar_) menu_bar_->draw_menu(*this);
        });

    auto& playback_group = dock_manager->add_group("playback_group", {
        .position = DockPosition::Top,
        .fixed = true,
        .resizable = false,
        .extent = px(TOOLBAR_ROW_H * 2.0),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    playback_group.add_panel<FnDock>("playback_panel", "Playback",
        DockPanelConfig{ .is_flex = true, .padding = {8.0f, 4.0f} },
        [this] {
            draw_panel_fancy_playback(*this);
        });

    auto& stats_group = dock_manager->add_group("stats_group", {
        .position = DockPosition::Bottom,
        .fixed = true,
        .resizable = false,
        .extent = px(TOOLBAR_ROW_H),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    stats_group.add_panel<FnDock>("stats_bar", "Statistics",
        DockPanelConfig{ .is_flex = true, .padding = { 8.0f, 4.0f } },
        [this] { draw_panel_process_stats(*this); });

    auto& toolbar_group = dock_manager->add_group("toolbar_group", {
        .position = DockPosition::Top,
        .fixed = false,
        .resizable = false,
        .extent = px(TOOLBAR_ROW_H),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    toolbar_group.add_panel<FnDock>("editor_tools_toollist", "Editor tool list",
        DockPanelConfig{ .is_flex = false, .padding = { 8.0f, 4.0f } },
        [this] { draw_panel_editor_tools_toollist(*this); });

    toolbar_group.add_panel<FnDock>("editor_tools_notesnap", "Note snap",
        DockPanelConfig{ .is_flex = false, .padding = { 8.0f, 4.0f } },
        [this] { draw_panel_editor_tools_note_snap(*this); });

    toolbar_group.add_panel<FnDock>("editor_tools_noteproperties", "Note properties",
        DockPanelConfig{ .is_flex = false, .padding = { 8.0f, 4.0f } },
        [this] { draw_panel_editor_tools_note_properties(*this); });

    toolbar_group.add_panel<FnDock>("editor_tool_trackoptions", "Track options",
        DockPanelConfig{ .is_flex = false, .padding = { 8.0f, 4.0f } },
        [this] { draw_panel_editor_tools_track_options(*this); });

    toolbar_group.add_panel<FnDock>("editor_tool_zoomcontrols", "Zoom controls",
        DockPanelConfig{ .is_flex = false, .padding = { 8.04f, 4.0f} },
        [this] { draw_panel_editor_tools_zoom_controls(*this); });

    toolbar_group.add_panel<FnDock>("editor_tool_playback_stub", "Misc Playback controls",
        DockPanelConfig{ .is_flex = false, .padding = { 8.04f, 4.0f} },
        [this] { draw_panel_playback_buttons(*this); });

    /*auto& playback_group = dock_manager->add_group("playback_group", {
        .position = DockPosition::Top,
        .fixed = false,
        .resizable = false,
        .extent = px(TOOLBAR_ROW_H),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    playback_group.add_panel<FnDock>("playback_buttons", "Playback",
        DockPanelConfig{ .is_flex = false, .padding = {8.0f, 4.0f} },
        [this] { draw_panel_playback_buttons(*this); });*/

    auto& side_group = dock_manager->add_group("side_controls_group", {
        .position = DockPosition::Right,
        .fixed = false,
        .resizable = false,
        .extent = px(40.0),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    side_group.add_panel<FnDock>("side_controls", "Editor controls",
        DockPanelConfig{ .is_flex = false, .padding = {4.0f, 4.0f} },
        [this] { draw_panel_side_controls(*this); });

    auto& track_list_group = dock_manager->add_group("track_list_group", {
        .position = DockPosition::Left,
        .fixed = true,
        .resizable = true,
        .extent = px(240),
        });

    track_list_group.add_panel<FnDock>("track_list", "Track list",
        DockPanelConfig{ .is_flex = true, .padding = {8.0f, 4.0f} },
        [this] { draw_panel_track_list(*this); });

    auto& playhead_group = dock_manager->add_group("playhead_group", {
        .position = DockPosition::Top,
        .fixed = true,
        .resizable = false,
        .extent = px(TOOLBAR_ROW_H),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    playhead_group.add_panel<FnDock>("playhead", "Playhead",
        DockPanelConfig{ .is_flex = false, .padding = {2.0f, 0.0f} },
        [this] { draw_panel_playhead_ui(*this); });

    auto& track_scroll_group = dock_manager->add_group("track_scroll_group", {
        .position = DockPosition::Right,
        .fixed = true,
        .resizable = false,
        .extent = px(13.0),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    track_scroll_group.add_panel<FnDock>("track_scroll", "Track Scroll",
        DockPanelConfig{ .is_flex = false, .padding = {0.0f, 0.0f} },
        [this] { draw_panel_scroll_navigation_vertical(*this); },
        [this] { return render_type_ == RenderType::TrackView; });

    auto& scroll_navigation_group = dock_manager->add_group("scroll_navigation_group", {
        .position = DockPosition::Bottom,
        .fixed = true,
        .resizable = false,
        .extent = px(13.0),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    scroll_navigation_group.add_panel<FnDock>("scroll_navigation", "Scroll navigation",
        DockPanelConfig{ .is_flex = false, .padding = {0.0f, 0.0f} },
        [this] { draw_panel_scroll_navigation(*this); });

    auto& data_view_group = dock_manager->add_group("data_view_group", {
        .position = DockPosition::Bottom,
        .fixed = false,
        .resizable = true,
        .extent_fn = [this]{ return static_cast<float>(view_settings->value.pr_dataview_size); },
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    data_view_group.add_panel<FnDock>("data_view", "Data viewer",
        DockPanelConfig{ .is_flex = true, .padding = {0.0f, 4.0f} },
        [this] { draw_panel_data_viewer(*this); },
        [this] { return view_settings->value.pr_dataview_state != VS_PianoRoll_DataViewState::Hidden && render_type_ != RenderType::TrackView; });

    auto& bar_number_group = dock_manager->add_group("bar_number_group", {
        .position = DockPosition::Top,
        .fixed = true,
        .resizable = false,
        .extent = px(BAR_NUMBER_ROW_H),
        .window_flags = ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse
        });

    bar_number_group.add_panel<FnDock>("bar_number", "Bar numbers",
        DockPanelConfig{ .is_flex = true, .padding = {0.0f, 0.0f} },
        [this] { draw_panel_bar_numbers(*this); });
}
} // namespace andromeda::app
