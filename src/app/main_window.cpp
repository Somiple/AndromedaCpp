#include "app/main_window.h"

#include <glad/glad.h>
// glad must be included before glfw
#include <GLFW/glfw3.h>
#include <imgui.h>
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

#include "app/rendering/data_view.h"
#include "app/rendering/note_gpu_cache.h"
#include "app/rendering/note_occlusion.h"
#include "app/rendering/note_uploader.h"
#include "app/rendering/piano_roll.h"
#include "app/rendering/screen_coverage.h"
#include "app/theme.h"
#include "app/ui/dialogs/crash_dialog.h"
#include "app/ui/dialogs/filter_channels.h"
#include "app/ui/dialogs/simple_dialog.h"
#include "app/ui/editor_info.h"
#include "app/ui/main_menu_bar.h"
#include "app/ui/manual.h"
#include "app/ui/panels.h"
#include "app/ui/dock_panels/dock_panel.h"
#include "app/widgets/data_view_widget.h"
#include "app/widgets/piano_roll_widget.h"
#include "app/widgets/track_view_widget.h"
#include "audio/kdmapi_engine.h"
#include "audio/midi_devices.h"
#include "editor/plugins/plugin_andromeda_obj.h"
#include "editor/plugins/plugin_dialog.h"
#include "editor/plugins/plugin_error_dialog.h"
#include "editor/settings/editor_settings.h"
#include "editor/settings/project_settings.h"
#include "editor/editor_controller.h"
#include "editor/util.h"
#include "midi/midi_file.h"
#include "util/crash_handler.h"
#include "util/debugger.h"

namespace andromeda::app {

using util::Debugger;

namespace {

constexpr float TOOLBAR_ROW_H = 25.0f;
constexpr float TRANSPORT_ROW_H = 23.0f;
constexpr float SLIDER_ROW_H = 22.0f;
constexpr float BAR_NUMBER_ROW_H = 26.0f;

}

MainWindow::MainWindow()
    : editor_controller(this),
      nav(util::make_shared_rw<editor::PianoRollNavigation>()),
      track_nav(util::make_shared_rw<editor::TrackViewNavigation>()),
      view_settings(util::make_shared_mut<ViewSettings>()) {

    editor::ProjectManager* project_manager = editor_controller.get_project_manager();
    project_manager->new_empty_project();
    bar_cacher = std::make_shared<editor::BarCacher>(project_manager);

    auto device = std::make_shared<util::SharedMut<std::shared_ptr<audio::MIDIAudioEngine>>>();
    device->value = std::make_shared<audio::kdmapi::KDMAPI>();
    kdmapi_slot = device;

    if (auto devices = audio::MIDIDevices::create()) {
        midi_devices = std::shared_ptr<audio::MIDIDevices>(std::move(*devices));
        midi_devices_slot =
            std::make_shared<util::SharedMut<std::shared_ptr<audio::MIDIAudioEngine>>>();
        midi_devices_slot->value = midi_devices;
    } else {
        Debugger::log_warning(
            std::format("MIDI I/O is unavailable: {}", devices.error()));
    }

    std::thread([device]() {
        std::lock_guard lock(device->mutex);
        device->value->init_audio();
    }).detach();

    std::vector<midi::MetaEvent>* metas = project_manager->get_metas();
    std::vector<midi::MIDITrack>* tracks = project_manager->get_tracks();
    editor::TempoMap* tempo_map = project_manager->get_tempo_map();

    realtime_engine = std::make_shared<audio::PlaybackManager>(device, tracks, metas, tempo_map);
    audio_engine = realtime_engine;

    {
        const char* engine_env = std::getenv("ANDROMEDA_AUDIO_ENGINE");
        const bool want_prerender =
            engine_env != nullptr && std::string_view(engine_env) == "prerender";

        if (want_prerender) {
            prerender_engine = std::make_shared<audio::PrerenderEngine>(tracks, tempo_map);
            prerender_engine->init();

            if (prerender_engine->is_available()) {
                audio_engine = prerender_engine;
                Debugger::log("Using the prerendered audio engine.");
            } else {
                Debugger::log_warning(
                    "Prerendered audio is unavailable, using the realtime engine instead");
                prerender_engine.reset();
            }
        }
    }

    midi_io = MIDIIoHandler(project_manager);

    {
        auto engine_handle =
            std::make_shared<util::SharedMut<std::shared_ptr<audio::AudioEngine>>>();
        engine_handle->value = audio_engine;
        playhead = std::make_shared<editor::Playhead>(0, std::move(engine_handle));
    }

    plugin_loader = std::make_unique<editor::PluginLoader>("assets/plugins/custom",
                                                           "assets/plugins/builtin");
    if (auto result = plugin_loader->load_all_plugins(); !result) {
        Debugger::log_warning(std::format("Custom plugins were not loaded: {}", result.error()));
    }
}

MainWindow::~MainWindow() = default;

void MainWindow::make_new_project() {
    const bool is_empty = editor_controller.get_project_manager()->is_project_empty(false);

    if (is_empty) {
        return;
    }

    DialogArgs args;
    args.emplace_back(std::string("Confirmation"));
    args.emplace_back(std::string("Are you sure you want to start a new project?"));
    args.emplace_back(std::string("NewProjectConfirmation"));

    show_dialog_with_args(dialog_names::DIALOG_NAME_SIMPLE, std::move(args));
}

void MainWindow::save_project() {
    status_text_ = "Save Project is not wired up yet (needs the file dialog)";
    Debugger::log_warning(status_text_);
}

void MainWindow::import_midi() { midi_io.rfd_import_midi(); }

void MainWindow::export_midi() { midi_io.rfd_export_midi(); }

void MainWindow::on_midi_loaded(midi::MIDIParseStatus import_status) {
    switch (import_status) {
    case midi::MIDIParseStatus::ParseOK: {
        editor::ProjectManager* manager = editor_controller.get_project_manager();
        {
            update_global_ppq(manager->get_ppq());
        }

        if (audio_engine->is_playing()) {
            audio_engine->toggle_playback();
            audio_engine->reset_events();
        }

        // fixed rust bug: a new file kept the old playhead and view, so it played from there
        playhead->set_start(0);
        {
            std::unique_lock lock(nav->mutex);
            nav->value.tick_pos = 0.0f;
            nav->value.tick_pos_smoothed = 0.0f;
        }
        {
            std::unique_lock lock(track_nav->mutex);
            track_nav->value.tick_pos = 0.0f;
            track_nav->value.tick_pos_smoothed = 0.0f;
        }

        editor_controller.get_actions()->clear_actions();

        if (realtime_engine) {
            realtime_engine->prewarm_priorities();
        }

        // stop the uploader before dropping the caches; it writes into them
        note_uploader.stop();
        if (piano_roll_widget_ != nullptr) {
            piano_roll_widget_->renderer().drop_note_caches();
        }

        {
            note_uploader.prewarm(manager->get_tracks());
        }

        status_text_ = "MIDI imported";
        break;
    }
    case midi::MIDIParseStatus::ParseError: {
        status_text_ = "The MIDI did not load correctly.";
        Debugger::log_error(status_text_);

        DialogArgs args;
        args.emplace_back(std::string("MIDI failed to import"));
        args.emplace_back(std::string("The MIDI did not load correctly."));
        args.emplace_back(std::string("MIDILoadError"));
        args.emplace_back(false);
        show_dialog_with_args(dialog_names::DIALOG_NAME_SIMPLE, std::move(args));
        break;
    }
    default:
        break;
    }
}

void MainWindow::update_global_ppq(std::uint16_t ppq) {
    send_event_to_listeners(PPQChanged{ppq});
}

void MainWindow::send_event_to_listeners(const AndromedaEvent& event) {
    app_event_handler.send_event_to_listeners(event);
}

void MainWindow::show_dialog(const char* name) { show_dialog_with_args(name, {}); }

void MainWindow::show_dialog_with_args(const char* name, DialogArgs args) {
    dialog_manager->close_all_dialogs();
    dialog_manager->open_dialog_by_name(name, std::move(args));
}

void MainWindow::init_dialogs() {
    using namespace dialog_names;

    dialog_drawer.init(dialog_manager);
    dialog_drawer.on_terminate_app = [this]() {
        if (window_ != nullptr) {
            glfwSetWindowShouldClose(window_, GLFW_TRUE);
        }
    };

    dialog_manager->register_dialog(DIALOG_NAME_SIMPLE,
                                    []() { return std::make_unique<SimpleDialog>(); });
    dialog_manager->register_dialog(DIALOG_NAME_EDITOR_MANUAL,
                                    []() { return std::make_unique<EditorManualDialog>(); });
    dialog_manager->register_dialog(DIALOG_NAME_EDITOR_INFO,
                                    []() { return std::make_unique<EditorInfo>(); });
    dialog_manager->register_dialog(DIALOG_NAME_INSERT_META, []() {
        return std::make_unique<editor::MetaEventInsertDialog>();
    });
    dialog_manager->register_dialog(DIALOG_NAME_PLUGIN_ERROR_DIALOG, []() {
        return std::make_unique<editor::PluginErrorDialog>();
    });
    dialog_manager->register_dialog(DIALOG_NAME_FILTER_CHANNELS,
                                    []() { return std::make_unique<FilterChannelsDialog>(); });
    dialog_manager->register_dialog(DIALOG_NAME_CRASH,
                                    []() { return std::make_unique<CrashDialog>(); });


    dialog_manager->register_dialog(DIALOG_NAME_EF_STRETCH, [this]() {
        return std::make_unique<editor::EFStretchDialog>(&editor_controller);
    });

    dialog_manager->register_dialog(DIALOG_NAME_EF_CHOP, [this]() {
        return std::make_unique<editor::EFChopDialog>(&editor_controller);
    });
    dialog_manager->register_dialog(DIALOG_NAME_EF_GLUE, [this]() {
        return std::make_unique<editor::EFGlueDialog>(&editor_controller);
    });
    dialog_manager->register_dialog(DIALOG_NAME_EF_SET_CHANNEL, [this]() {
        return std::make_unique<editor::EFSetChannelDialog>(&editor_controller);
    });

    dialog_manager->register_dialog(DIALOG_NAME_PROJECT_SETTINGS, [this]() {
        return std::make_unique<editor::ProjectSettings>(editor_controller.get_project_manager());
    });

    dialog_manager->register_dialog(DIALOG_NAME_EDITOR_SETTINGS, [this]() {
        auto settings = std::make_unique<editor::ESSettingsWindow>();
        if (midi_devices && midi_devices_slot) {
            settings->use_midi_devices(midi_devices, midi_devices_slot);
        }
        if (kdmapi_slot) {
            settings->use_kdmapi(kdmapi_slot);
        }
        settings->use_playback_manager(audio_engine);

        auto active = editor::ESAudioEngineType::KDMAPI;
        if (prerender_engine && audio_engine == prerender_engine) {
            active = editor::ESAudioEngineType::Prerendered;
        } else if (realtime_engine && midi_devices_slot &&
                   realtime_engine->device == midi_devices_slot) {
            active = editor::ESAudioEngineType::MidiIO;
        }
        settings->set_active_engine(active);
        return settings;
    });

    dialog_manager->register_dialog(DIALOG_NAME_PLUGIN_DIALOG, [this]() {
        auto plugin_dialog = std::make_unique<editor::PluginDialog>();
        plugin_dialog->init(&editor_controller);
        return plugin_dialog;
    });
}

void MainWindow::process_closed_dialogs() {
    std::unique_ptr<Dialog> closed = dialog_manager->take_last_closed_dialog();
    if (!closed) {
        return;
    }

    auto* simple = dynamic_cast<SimpleDialog*>(closed.get());
    if (simple == nullptr || !simple->ok_clicked) {
        return;
    }

    if (simple->id == "NewProjectConfirmation") {
        {
            Debugger::log("Clearning notes...");
            editor_controller.get_project_manager()->new_empty_project();
        }

        Debugger::log("Removing action history...");
        editor_controller.get_actions()->clear_actions();

        Debugger::log("Stopping playback (if any)...");
        if (audio_engine->is_playing()) {
            audio_engine->toggle_playback();
        }

        playhead->set_start(0);
        {
            std::unique_lock lock(nav->mutex);
            nav->value.tick_pos = 0.0f;
        }

        status_text_ = "New project";
    } else {
        Debugger::log_warning(
            std::format("Don't know what to do with Simple Dialog {}", simple->id));
    }
}

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
        [this] { return render_type == RenderType::TrackView; });

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
        [this] { return view_settings->value.pr_dataview_state != VS_PianoRoll_DataViewState::Hidden && render_type != RenderType::TrackView; });

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

void MainWindow::apply_function(editor::EditFunction function_type) {
    using namespace dialog_names;

    if (std::holds_alternative<editor::edit_fn::Stretch>(function_type)) {
        show_dialog(DIALOG_NAME_EF_STRETCH);
        return;
    }
    if (std::holds_alternative<editor::edit_fn::Chop>(function_type)) {
        show_dialog(DIALOG_NAME_EF_CHOP);
        return;
    }
    if (std::holds_alternative<editor::edit_fn::Glue>(function_type)) {
        show_dialog(DIALOG_NAME_EF_GLUE);
        return;
    }
    if (std::holds_alternative<editor::edit_fn::SetChannel>(function_type)) {
        show_dialog(DIALOG_NAME_EF_SET_CHANNEL);
        return;
    }

    editor_controller.perform_function(function_type);
}

void MainWindow::insert_meta(midi::MetaEventType meta_type) {
    if (meta_type != midi::MetaEventType::TimeSignature &&
        meta_type != midi::MetaEventType::Tempo) {
        return;
    }

    auto meta_dialog = std::make_unique<editor::MetaEventInsertDialog>();
    const editor::MIDITick playhead_pos = playhead->start_tick;
    auto meta_editing = editor_controller.get_meta_editing();

    meta_dialog->init_meta_dialog(meta_type, [meta_editing, playhead_pos, meta_type](
                                                 std::vector<std::uint8_t> data) {
            meta_editing->insert_meta_event(
            midi::MetaEvent{playhead_pos, meta_type, std::move(data)});
    });

    dialog_manager->open_dialog(std::move(meta_dialog), {});
}

void MainWindow::filter_selection_channels() {
    DialogArgs args;
    editor::EditorController* controller = &editor_controller;
    args.emplace_back(controller);

    dialog_manager->open_dialog_by_name(dialog_names::DIALOG_NAME_FILTER_CHANNELS,
                                        std::move(args));
}

void MainWindow::run_plugin(std::shared_ptr<editor::PluginLua> plugin) {
    if (!plugin) {
        return;
    }

    const std::shared_ptr<sol::state> lua = plugin->lua();
    const std::size_t track_idx = editor_controller.get_active_track();
    (*lua)["curr_track"] = track_idx;

    editor::AndromedaObj::register_type(*lua);
    (*lua)["andromeda"] =
        std::make_shared<editor::AndromedaObj>(editor_controller.get_project_manager(), playhead);

    auto plugin_dialog = std::make_unique<editor::PluginDialog>();
    plugin_dialog->init(&editor_controller);
    plugin_dialog->curr_track = track_idx;

    std::string error_msg;
    if (auto should_show_dialog = plugin_dialog->load_plugin_dialog(plugin)) {
        if (*should_show_dialog) {
            dialog_manager->open_dialog(std::move(plugin_dialog), {});
            return;
        }

        if (auto run_result = plugin_dialog->run_plugin(); !run_result) {
            error_msg = run_result.error();
        }
    } else {
        error_msg = should_show_dialog.error();
    }

    if (!error_msg.empty()) {
        Debugger::log_error("Lua plugin failed to run!");
        DialogArgs args;
        args.emplace_back(plugin->plugin_name);
        args.emplace_back(error_msg);
        dialog_manager->open_dialog(std::make_unique<editor::PluginErrorDialog>(),
                                    std::move(args));
    }
}

// the seek point, or while playing without autoscroll the moving playhead, when it is on screen
std::optional<float> MainWindow::zoom_anchor_tick(float view_pos, float view_zoom) const {
    bool autoscroll = false;
    {
        std::lock_guard lock(view_settings->mutex);
        autoscroll = view_settings->value.pr_autoscroll;
    }

    // while autoscrolling the playhead sits where the start tick sits in the unscrolled view
    const float anchor = static_cast<float>(audio_engine->is_playing() && !autoscroll
                                                ? audio_engine->get_playback_ticks()
                                                : audio_engine->get_playback_start_tick());
    if (anchor < view_pos || anchor > view_pos + view_zoom) {
        return std::nullopt;
    }
    return anchor;
}

EditorWidget& MainWindow::active_view() const {
    if (render_type == RenderType::TrackView) {
        return *track_view_widget_;
    }
    return *piano_roll_widget_;
}

EditorWidget& MainWindow::data_view() const { return *data_view_widget_; }

void MainWindow::curr_view_zoom_in_by(float x_fac, float y_fac) {
    active_view().zoom_by(x_fac, y_fac);
}

void MainWindow::on_current_track_changed(std::uint16_t track) {
    Debugger::log("Track changed");
    {
        editor_controller.get_project_manager()->get_project_data_mut().validate_tracks(track);
    }
    std::unique_lock lock(nav->mutex);
    nav->value.curr_track = track;
}

std::uint16_t MainWindow::get_ppq() {
    return editor_controller.get_project_manager()->get_ppq();
}

float MainWindow::get_keyboard_width() const { return active_view().keyboard_width(); }

// stopping returns the playhead to where playback started; autoscroll never moved the view
// itself, so leaving it alone brings it back there too
void MainWindow::toggle_playback_from_start() {
    if (!audio_engine->is_playing()) {
        // fixed rust bug: playing with the playhead scrolled out of view kept the view away
        show_tick(audio_engine->get_playback_start_tick());
        audio_engine->toggle_playback();
        return;
    }

    audio_engine->toggle_playback();
    // zooming while playing may have left the view before 0, which a stopped view cannot show
    {
        std::unique_lock lock(nav->mutex);
        nav->value.tick_pos = std::max(0.0f, nav->value.tick_pos);
        nav->value.tick_pos_smoothed = std::max(0.0f, nav->value.tick_pos_smoothed);
    }
    {
        std::unique_lock lock(track_nav->mutex);
        track_nav->value.tick_pos = std::max(0.0f, track_nav->value.tick_pos);
        track_nav->value.tick_pos_smoothed = std::max(0.0f, track_nav->value.tick_pos_smoothed);
    }
}

// the smoothed position too: renderers take their playback offset from it on the first frame
void MainWindow::show_tick(editor::MIDITick tick) {
    const auto [min_tick, max_tick] = get_view_tick_range();
    if (tick >= min_tick && tick < max_tick) {
        return;
    }

    const float lead = static_cast<float>(max_tick - min_tick) / 16.0f;
    active_view().jump_to_tick(std::max(0.0f, static_cast<float>(tick) - lead));
}

bool MainWindow::wants_continuous_frames() const {
    if (audio_engine && audio_engine->is_playing()) {
        return true;
    }
    {
        std::shared_lock pr_lock(nav->mutex);
        std::shared_lock tv_lock(track_nav->mutex);
        if (nav->value.smoothed_values_needs_update() ||
            track_nav->value.smoothed_values_needs_update()) {
            return true;
        }
    }
    const ImGuiIO& io = ImGui::GetIO();
    return io.WantTextInput || ImGui::IsAnyMouseDown();
}

void MainWindow::switch_view(RenderType to) {
    {
        std::unique_lock pr_lock(nav->mutex);
        std::unique_lock tv_lock(track_nav->mutex);
        if (to == RenderType::TrackView) {
            track_nav->value.tick_pos = nav->value.tick_pos;
            track_nav->value.tick_pos_smoothed = nav->value.tick_pos_smoothed;
        } else {
            nav->value.tick_pos = track_nav->value.tick_pos;
            nav->value.tick_pos_smoothed = track_nav->value.tick_pos_smoothed;
        }
    }

    set_render_type(to);

    if (to == RenderType::PianoRoll) {
        data_view_widget_->target_renderer->set_active(true);
    }
}

void MainWindow::set_render_type(RenderType to) {
    EditorWidget* const shown = to == RenderType::TrackView
                                    ? static_cast<EditorWidget*>(track_view_widget_)
                                    : static_cast<EditorWidget*>(piano_roll_widget_);
    EditorWidget* const hidden = to == RenderType::TrackView
                                     ? static_cast<EditorWidget*>(piano_roll_widget_)
                                     : static_cast<EditorWidget*>(track_view_widget_);

    hidden->target_renderer->set_active(false);
    render_type = to;
    shown->target_renderer->set_active(true);
}

std::pair<editor::MIDITick, editor::MIDITick> MainWindow::get_view_tick_range_with_playback()
    const {
    const bool is_playing = audio_engine->is_playing();

    bool autoscroll = false;
    {
        std::lock_guard lock(view_settings->mutex);
        autoscroll = view_settings->value.pr_autoscroll;
    }

    if (!is_playing || !autoscroll) {
        return get_view_tick_range();
    }

    const auto [pos, zoom] = active_view().tick_view();

    // same as the renderers: the unscrolled view may sit before 0 while playing (zoom keeps
    // the playhead in place); only the view on screen stops at the song start
    const float playback = static_cast<float>(audio_engine->get_playback_ticks());
    const float start = static_cast<float>(audio_engine->get_playback_start_tick());
    const float min = std::max(0.0f, playback + pos - start);
    return {static_cast<editor::MIDITick>(min), static_cast<editor::MIDITick>(min + zoom)};
}

float MainWindow::get_playhead_pos(bool to_window) const {
    float playhead_line_pos = static_cast<float>(playhead->start_tick);

    if (audio_engine->is_playing()) {
        playhead_line_pos = static_cast<float>(audio_engine->get_playback_ticks());
    }

    const float tick_pos_smoothed = active_view().tick_view().pos;

    return to_window ? playhead_line_pos - tick_pos_smoothed : playhead_line_pos;
}

std::pair<editor::MIDITick, editor::MIDITick> MainWindow::get_view_tick_range() const {
    const auto [pos, zoom] = active_view().tick_view();

    return {static_cast<editor::MIDITick>(std::max(0.0f, pos)),
            static_cast<editor::MIDITick>(std::max(0.0f, pos + zoom))};
}

void MainWindow::load_image_resources() {
    static constexpr const char* ICONS[] = {
        "pencil", "eraser", "select",     "copy",       "cut",        "paste",
        "undo",   "redo",   "zoom_x_in",  "zoom_x_out", "zoom_y_in",  "zoom_y_out",
        "logo",   "logo_small", "logo_medium",
    };

    for (const char* id : ICONS) {
        image_resources.preload_image(std::format("assets/icons/{}.png", id), id);
    }
}

void MainWindow::build_menu_bar() {
    menu_bar_ = std::make_unique<MainMenuBar>();

    if (const ImageHandle logo = image_resources.get_image_handle("logo_small"); logo.valid()) {
        menu_bar_->add_menu_image_action(logo.texture_id, 16.0f, 16.0f,
                                         [](MainWindow& mw) { mw.show_dialog(dialog_names::DIALOG_NAME_EDITOR_INFO); });
    }

    menu_bar_->add_menu("File", {
        {"New Project", MenuButton{[](MainWindow& mw) { mw.make_new_project(); }}},
        {"Save Project (WIP)", MenuButton{[](MainWindow& mw) { mw.save_project(); }}},
        {"", MenuSeparator{}},
        {"Import MIDI file", MenuButton{[](MainWindow& mw) { mw.import_midi(); }}},
        {"Export MIDI file", MenuButton{[](MainWindow& mw) { mw.export_midi(); }}},
    });

    const auto note_selected = [](MainWindow& mw) { return mw.editor_controller.get_selection()->is_any_note_selected(); };

    menu_bar_->add_menu("Edit", {
        {"Undo", MenuButtonEnabled{[](MainWindow& mw) { mw.editor_controller.undo(); },
                                   [](MainWindow& mw) { return mw.editor_controller.can_undo(); }}},
        {"Redo", MenuButtonEnabled{[](MainWindow& mw) { mw.editor_controller.redo(); },
                                   [](MainWindow& mw) { return mw.editor_controller.can_redo(); }}},
        {"", MenuSeparator{}},
        {"Insert...", SubMenu{{
            {"Time Signature", MenuButton{[](MainWindow& mw) {
                 mw.insert_meta(midi::MetaEventType::TimeSignature);
             }}},
            {"Tempo", MenuButton{[](MainWindow& mw) {
                 mw.insert_meta(midi::MetaEventType::Tempo);
             }}},
        }}},
        {"", MenuSeparator{}},
        {"Copy", MenuButtonEnabled{[](MainWindow& mw) { mw.editor_controller.copy(); },
                                   [](MainWindow& mw) { return mw.editor_controller.can_copy(); }}},
        {"Cut", MenuButtonEnabled{[](MainWindow& mw) { mw.editor_controller.cut(); },
                                  [](MainWindow& mw) { return mw.editor_controller.can_copy(); }}},
        {"Paste", MenuButtonEnabled{[](MainWindow& mw) { mw.editor_controller.paste(); },
                                    [](MainWindow& mw) { return mw.editor_controller.can_paste(); }}},
        {"", MenuSeparator{}},
        {"Select...", SubMenu{{
            {"Filter Selection...", SubMenu{{
                {"Filter channnels", MenuButtonEnabled{
                    [](MainWindow& mw) { mw.filter_selection_channels(); }, note_selected}},
            }}},
        }}},
        {"", MenuSeparator{}},
        {"Transpose up", MenuButtonEnabled{[](MainWindow& mw) {
                                               mw.apply_function(editor::edit_fn::Transpose{1});
                                           },
                                           note_selected}},
        {"Transpose down", MenuButtonEnabled{[](MainWindow& mw) {
                                                 mw.apply_function(editor::edit_fn::Transpose{-1});
                                             },
                                             note_selected}},
        {"", MenuSeparator{}},
        {"+1 Octave", MenuButtonEnabled{[](MainWindow& mw) {
                                            mw.apply_function(editor::edit_fn::Transpose{12});
                                        },
                                        note_selected}},
        {"-1 Octave", MenuButtonEnabled{[](MainWindow& mw) {
                                            mw.apply_function(editor::edit_fn::Transpose{-12});
                                        },
                                        note_selected}},
    });

    menu_bar_->add_menu("View", {
        {"Reset layout", MenuButton{[](MainWindow& mw) {
            mw.reset_ui_layout();
        }}},
    });

    menu_bar_->add_menu("Options", {
        {"Preferences...", MenuButton{[](MainWindow& mw) {
             mw.show_dialog(dialog_names::DIALOG_NAME_EDITOR_SETTINGS);
         }}},
    });

    menu_bar_->add_menu("Project", {
        {"Project settings...", MenuButton{[](MainWindow& mw) {
             mw.show_dialog(dialog_names::DIALOG_NAME_PROJECT_SETTINGS);
         }}},
    });

    std::vector<std::pair<std::string, MenuItem>> manip_plugin_buttons;
    std::vector<std::pair<std::string, MenuItem>> gen_plugin_buttons;
    if (plugin_loader) {
        for (const auto& plugin : plugin_loader->manip_plugins) {
            manip_plugin_buttons.emplace_back(
                plugin->plugin_name,
                MenuButton{[plugin](MainWindow& mw) { mw.run_plugin(plugin); }});
        }
        for (const auto& plugin : plugin_loader->gen_plugins) {
            gen_plugin_buttons.emplace_back(
                plugin->plugin_name,
                MenuButton{[plugin](MainWindow& mw) { mw.run_plugin(plugin); }});
        }
    }

    std::vector<std::pair<std::string, MenuItem>> plugin_menu{
        {"Manipulate...", SubMenu{std::move(manip_plugin_buttons)}},
        {"Generate...", SubMenu{std::move(gen_plugin_buttons)}},
        {"", MenuSeparator{}},
        {"Reload all plugins",
         MenuButtonWithTooltip{
             "Only reloads the plugins andromeda has loaded at startup (plugins that were added "
             "after startup are not added, therefore a restart is required for newly added "
             "plugins).",
             [](MainWindow& mw) {
                 if (mw.plugin_loader) {
                     if (auto result = mw.plugin_loader->reload_plugins(); !result) {
                         Debugger::log_error(result.error());
                     }
                 }
             }}},
        {"Open plugin folder", MenuButton{[](MainWindow&) {
             const std::filesystem::path path = "assets/plugins/custom";
             std::error_code ec;
             if (!std::filesystem::exists(path, ec)) {
                 std::filesystem::create_directories(path, ec);
                 if (ec) {
                     // fixed rust bug: panicked when the folder could not be created
                     Debugger::log_error(std::format("Failed to create directory {}: {}",
                                                     path.string(), ec.message()));
                     return;
                 }
             }

             const std::string command =
                 std::format("explorer \"{}\"", std::filesystem::absolute(path).string());
             std::system(command.c_str());
         }}},
    };

    menu_bar_->add_menu("Tools", {
        {"Editing", SubMenu{{
            {"Stretch selection...", MenuButtonEnabled{
                [](MainWindow& mw) {
                    mw.apply_function(editor::edit_fn::Stretch{{}, 0.0f});
                }, note_selected}},
            {"Chop selection...", MenuButtonEnabled{
                [](MainWindow& mw) { mw.apply_function(editor::edit_fn::Chop{{}, 0}); },
                note_selected}},
            {"Slice notes at playhead", MenuButtonEnabled{
                [](MainWindow& mw) {
                    mw.apply_function(editor::edit_fn::SliceAtTick{{}, mw.playhead->start_tick});
                }, note_selected}},
            {"Glue notes...", MenuButtonEnabled{
                [](MainWindow& mw) {
                    mw.apply_function(editor::edit_fn::Glue{{}, 0, false});
                }, note_selected}},
            {"Remove Overlaps", MenuButtonEnabled{
                [](MainWindow& mw) { mw.apply_function(editor::edit_fn::RemoveOverlaps{}); },
                note_selected}},
            {"Set channel of notes...", MenuButtonEnabled{
                [](MainWindow& mw) { mw.apply_function(editor::edit_fn::SetChannel{0}); },
                note_selected}},
            {"", MenuSeparator{}},
            {"Fade In", MenuButtonEnabled{
                [](MainWindow& mw) { mw.apply_function(editor::edit_fn::FadeNotes{false}); },
                note_selected}},
            {"Fade Out", MenuButtonEnabled{
                [](MainWindow& mw) { mw.apply_function(editor::edit_fn::FadeNotes{true}); },
                note_selected}},
            {"", MenuSeparator{}},
            {"Plugins", SubMenu{std::move(plugin_menu)}},
        }}},
    });

    menu_bar_->add_menu("Help", {
        {"Manual", MenuButton{[](MainWindow& mw) {
             mw.show_dialog(dialog_names::DIALOG_NAME_EDITOR_MANUAL);
         }}},
    });

    menu_bar_->add_custom([this] {
        ImGui::Dummy({5.0f, 0.0f});
    });

    menu_bar_->add_custom([this] {
        std::string test = "";

        ImGui::SetNextItemWidth(200);
        ImGui::InputTextWithHint("##searchText", "Search...", test.data(), test.length(), 0, 0);
    });
}

void MainWindow::draw_ui() {
    dock_manager->begin_frame();
    dock_manager->draw();

    draw_central();

    update_smoothed_values();
}

void MainWindow::reset_ui_layout() {
    dock_manager->reset_layout();
}

void MainWindow::init_widgets() {
    // must run after glad has loaded the gl functions
    note_colors = std::make_shared<NoteColors>(NoteColors::create());

    editor::ProjectManager* manager = editor_controller.get_project_manager();
    note_culler = std::make_shared<rendering::NoteCullHelper>(manager->get_tracks());

    auto piano_roll = std::make_unique<PianoRollWidget>(*this);
    auto track_view = std::make_unique<TrackViewWidget>(*this);
    const std::shared_ptr<rendering::NoteGpuCache> note_cache = piano_roll->renderer().note_cache();
    auto data_view = std::make_unique<DataViewWidget>(*this, note_cache);

    piano_roll_widget_ = piano_roll.get();
    track_view_widget_ = track_view.get();
    data_view_widget_ = data_view.get();

    widgets.push_back(std::move(piano_roll));
    widgets.push_back(std::move(track_view));
    widgets.push_back(std::move(data_view));

    set_render_type(RenderType::PianoRoll);

    app_event_handler.register_listener(audio_engine);
    app_event_handler.register_listener(bar_cacher);
    app_event_handler.register_listener(data_view_widget_->renderer());

    note_uploader.init(window_, note_cache);

    update_global_ppq(get_ppq());
}

void MainWindow::update_input_state() {
    const auto& io = ImGui::GetIO();
    _mouse_pos = { io.MousePos.x, io.MousePos.y };

    _key_modifiers.shift = io.KeyShift;
    _key_modifiers.ctrl = io.KeyCtrl;
    _key_modifiers.alt = io.KeyAlt;
}

void MainWindow::run_render_bench(int frames) {
    glfwSwapInterval(0);
    update_input_state();

    float zoom_ticks = 0.0f;
    {
        std::unique_lock lock(nav->mutex);
        nav->value.tick_pos = 0.0f;
        nav->value.tick_pos_smoothed = 0.0f;

        if (startup_.bench_zoom_ticks > 0.0f) {
            nav->value.zoom_ticks = startup_.bench_zoom_ticks;
            nav->value.zoom_ticks_smoothed = startup_.bench_zoom_ticks;
        }
        if (startup_.bench_zoom_keys > 0.0f) {
            nav->value.zoom_keys = startup_.bench_zoom_keys;
            nav->value.zoom_keys_smoothed = startup_.bench_zoom_keys;
            const float key_pos = std::max(0.0f, 64.0f - startup_.bench_zoom_keys * 0.5f);
            nav->value.key_pos = key_pos;
            nav->value.key_pos_smoothed = key_pos;
        }

        zoom_ticks = nav->value.zoom_ticks_smoothed;
    }
    {
        std::lock_guard lock(view_settings->mutex);
        view_settings->value.pr_autoscroll = startup_.bench_play_at > 0.0f;

        view_settings->value.pr_onion_state = startup_.bench_no_onion
                                                  ? VS_PianoRoll_OnionState::NoOnion
                                                  : VS_PianoRoll_OnionState::ViewAll;
    }

    if (startup_.bench_no_onion) {
        std::size_t busiest = 0;
        std::size_t most = 0;
        {
            auto tracks = editor_controller.get_project_manager()->get_tracks();
            for (std::size_t t = 0; t < tracks->size(); ++t) {
                const std::size_t n = tracks->at(t).get_notes().size();
                if (n > most) {
                    most = n;
                    busiest = t;
                }
            }
        }
        std::printf("bench draws track %zu alone (%zu notes)\n", busiest, most);
        std::unique_lock lock(nav->mutex);
        nav->value.curr_track = static_cast<std::uint16_t>(busiest);
    }

    if (startup_.bench_hide_data_view) {
        std::lock_guard lock(view_settings->mutex);
        view_settings->value.pr_dataview_state = VS_PianoRoll_DataViewState::Hidden;
    }

    float song_ticks = 0.0f;
    {
        std::vector<midi::MIDITrack>& tracks = *editor_controller.get_project_manager()->get_tracks();
        for (midi::MIDITrack t : tracks) {
            if (!t.get_notes().empty()) {
                song_ticks = std::max(song_ticks, static_cast<float>(t.get_notes().back().start));
            }
        }
    }

    const float step = startup_.bench_still ? 0.0f : zoom_ticks / 60.0f;
    const float start_tick =
        std::max(0.0f, song_ticks * (startup_.bench_play_at > 0.0f ? startup_.bench_play_at : 0.4f));

    if (startup_.bench_play_at > 0.0f) {
        audio_engine->navigate_to(static_cast<editor::MIDITick>(start_tick));
        audio_engine->toggle_playback();
    }


    {
        std::shared_lock lock(nav->mutex);
        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        std::printf("view       %.0f ticks x %.1f keys from key %.1f, tick %.0f, %dx%d window\n",
                    nav->value.zoom_ticks_smoothed, nav->value.zoom_keys_smoothed,
                    nav->value.key_pos_smoothed, start_tick, fb_w, fb_h);
    }

    std::vector<double> gl_ms;
    std::vector<double> submit_ms;
    std::vector<double> frame_ms;
    std::uint64_t instances = 0;
    std::uint64_t coverage_tested = 0;
    std::uint64_t coverage_skipped = 0;
    std::uint64_t uploaded = 0;
    std::uint64_t resident_instances = 0;
    std::uint64_t cpu_instances = 0;
    int upload_frames = 0;
    std::size_t peak_instances = 0;
    double peak_instance_ms = 0.0;
    int peak_instance_frame = 0;

    struct FrameSample {
        double gl_ms = 0.0;
        double submit_ms = 0.0;
        std::size_t instances = 0;
        std::size_t uploaded = 0;
    };
    std::vector<FrameSample> samples;
    samples.reserve(static_cast<std::size_t>(frames));

    gl_ms.reserve(static_cast<std::size_t>(frames));
    frame_ms.reserve(static_cast<std::size_t>(frames));

    for (int i = 0; i < frames && glfwWindowShouldClose(window_) == GLFW_FALSE; ++i) {
        const auto frame_start = std::chrono::steady_clock::now();

        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        {
            std::unique_lock lock(nav->mutex);
            nav->value.tick_pos = start_tick + static_cast<float>(i) * step;
            nav->value.tick_pos_smoothed = nav->value.tick_pos;
        }

        mouse_over_ui = false;
        draw_ui();
        dialog_drawer.draw_all_dialogs(image_resources);
        ImGui::Render();

        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.04f, 0.04f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        const auto gl_start = std::chrono::steady_clock::now();
        draw_gl_surface();
        const auto submit_end = std::chrono::steady_clock::now();
        glFinish();
        const auto gl_end = std::chrono::steady_clock::now();

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);

        std::size_t uploaded_this_frame = 0;
        std::size_t drawn_this_frame = 0;
        if (auto* active = active_view().target_renderer.get()) {
            const std::size_t drawn = active->instances_drawn();
            drawn_this_frame = drawn;
            instances += drawn;
            if (drawn > peak_instances) {
                peak_instances = drawn;
                peak_instance_ms =
                    std::chrono::duration<double, std::milli>(gl_end - gl_start).count();
                peak_instance_frame = i;
            }
            resident_instances += active->resident_instances();
            cpu_instances += active->cpu_instances();
            coverage_tested += active->coverage_tested();
            coverage_skipped += active->coverage_skipped();
            uploaded_this_frame = active->take_uploaded_notes();
            uploaded += uploaded_this_frame;
            if (uploaded_this_frame > 0) {
                upload_frames += 1;
            }
        }

        constexpr int WARMUP_FRAMES = 10;
        if (i >= WARMUP_FRAMES) {
            gl_ms.push_back(std::chrono::duration<double, std::milli>(gl_end - gl_start).count());
            submit_ms.push_back(
                std::chrono::duration<double, std::milli>(submit_end - gl_start).count());
            samples.push_back(FrameSample{
                gl_ms.back(), submit_ms.back(),
                active_view().target_renderer.get() != nullptr ? drawn_this_frame : 0,
                uploaded_this_frame});
        }
        if (i >= 10) {
            frame_ms.push_back(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - frame_start)
                                   .count());
        }
    }

    if (startup_.bench_play_at > 0.0f) {
        audio_engine->toggle_playback();
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    if (gl_ms.empty()) {
        return;
    }

    if (!startup_.screenshot.empty()) {
        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w > 0 && h > 0) {
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) *
                                             static_cast<std::size_t>(h) * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_FRONT);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

            const std::size_t row = static_cast<std::size_t>(w) * 3;
            std::vector<std::uint8_t> flipped(pixels.size());
            for (int y = 0; y < h; ++y) {
                std::copy_n(pixels.begin() + static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(h - 1 - y)),
                            row, flipped.begin() + static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(y)));
            }

            if (stbi_write_png(startup_.screenshot.c_str(), w, h, 3, flipped.data(),
                               static_cast<int>(row)) == 0) {
                Debugger::log_error("Failed to write the bench screenshot");
            } else {
                std::printf("screenshot %s (%dx%d)\n", startup_.screenshot.c_str(), w, h);
            }
        }
    }

    const auto report = [](const char* what, std::vector<double> v) {
        std::sort(v.begin(), v.end());
        double sum = 0.0;
        for (const double x : v) {
            sum += x;
        }
        const double mean = sum / static_cast<double>(v.size());
        const double median = v[v.size() / 2];
        const double p95 = v[static_cast<std::size_t>(static_cast<double>(v.size()) * 0.95)];
        std::printf("%-10s mean %7.2f ms   median %7.2f ms   p95 %7.2f ms   max %7.2f ms   (%.0f fps at the mean)\n",
                    what, mean, median, p95, v.back(), 1000.0 / mean);
    };

    std::size_t total_notes = 0;
    {
        auto& tracks = *editor_controller.get_project_manager()->get_tracks();
        for (midi::MIDITrack& t : tracks) {
            total_notes += t.get_notes().size();
        }
    }

    std::printf("\n=== render bench: %d frames, %zu notes in project ===\n", frames, total_notes);
    std::printf("instances  %llu total, %.0f per frame\n",
                static_cast<unsigned long long>(instances),
                static_cast<double>(instances) / static_cast<double>(gl_ms.size()));
    std::printf("path       %llu instances from GPU-resident buffers, %llu packed by the CPU\n",
                static_cast<unsigned long long>(resident_instances),
                static_cast<unsigned long long>(cpu_instances));
    if (const auto* active = active_view().target_renderer.get()) {
        std::printf("residency  %zu MB resident; bails: %zu no buffer, %zu range past end, %zu empty range\n",
                    active->resident_megabytes(), active->residency_refusals(),
                    active->bail_range_past_end(), active->bail_empty_range());
        std::printf("tracks     %zu onion track draws resident, %zu fell back to the CPU\n",
                    active->resident_tracks(), active->cpu_tracks());
    }
    std::printf("coverage   %llu notes tested, %llu found hidden or off-screen (%.1f %%)\n",
                static_cast<unsigned long long>(coverage_tested),
                static_cast<unsigned long long>(coverage_skipped),
                coverage_tested > 0 ? 100.0 * static_cast<double>(coverage_skipped) /
                                          static_cast<double>(coverage_tested)
                                    : 0.0);
    std::printf("uploads    %llu notes over %d frames\n",
                static_cast<unsigned long long>(uploaded), upload_frames);
    std::printf("busiest    %zu instances in frame %d, which took %.2f ms\n", peak_instances,
                peak_instance_frame, peak_instance_ms);
    report("gl submit", submit_ms);
    report("gl total", gl_ms);
    report("frame", frame_ms);

    if (!samples.empty()) {
        std::vector<const FrameSample*> worst;
        worst.reserve(samples.size());
        for (const FrameSample& s : samples) {
            worst.push_back(&s);
        }
        std::partial_sort(worst.begin(),
                          worst.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(
                                              10, worst.size())),
                          worst.end(),
                          [](const FrameSample* a, const FrameSample* b) {
                              return a->gl_ms > b->gl_ms;
                          });

        std::printf("worst frames   %8s %8s %12s %12s\n", "gl ms", "submit", "instances",
                    "uploaded");
        for (std::size_t i = 0; i < std::min<std::size_t>(10, worst.size()); ++i) {
            std::printf("               %8.2f %8.2f %12zu %12zu\n", worst[i]->gl_ms,
                        worst[i]->submit_ms, worst[i]->instances, worst[i]->uploaded);
        }

        std::vector<const FrameSample*> busiest = worst;
        std::partial_sort(busiest.begin(),
                          busiest.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(
                                                5, busiest.size())),
                          busiest.end(),
                          [](const FrameSample* a, const FrameSample* b) {
                              return a->instances > b->instances;
                          });
        std::printf("busiest frames %8s %8s %12s %12s\n", "gl ms", "submit", "instances",
                    "uploaded");
        for (std::size_t i = 0; i < std::min<std::size_t>(5, busiest.size()); ++i) {
            std::printf("               %8.2f %8.2f %12zu %12zu\n", busiest[i]->gl_ms,
                        busiest[i]->submit_ms, busiest[i]->instances, busiest[i]->uploaded);
        }
    }
}

std::optional<GlSurface> MainWindow::gl_surface() const {
    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(window_, &fb_w, &fb_h);

    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) {
        return std::nullopt;
    }

    return GlSurface{fb_w, fb_h, static_cast<float>(fb_w) / display.x,
                     static_cast<float>(fb_h) / display.y};
}

void MainWindow::draw_gl_surface() {
    if (widgets.empty()) {
        return;
    }

    const std::optional<GlSurface> surface = gl_surface();
    if (!surface) {
        return;
    }

    // the data view pass is queued by its dock panel and runs with the imgui draw data
    active_view().draw_gl();

    // imgui does not reset these; a bound vao or program leaves the ui blank
    glBindVertexArray(0);
    glUseProgram(0);
    glViewport(0, 0, surface->fb_w, surface->fb_h);
}

void MainWindow::handle_key_inputs() {
    if (widgets.empty()) {
        return;
    }

    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        switch (render_type) {
        case RenderType::PianoRoll:
            switch_view(RenderType::TrackView);
            break;
        case RenderType::TrackView:
            switch_view(RenderType::PianoRoll);
            break;
        }
    }

    const ImGuiIO& io = ImGui::GetIO();

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Z, false)) {
        editor_controller.undo();
    }

    if (io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_Y, false)) {
        editor_controller.redo();
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Space, false)) {
        toggle_playback_from_start();
    }
}

void MainWindow::update_smoothed_values() {
    const float dt = std::min(timer_.get_delta_time(), 0.05f);

    std::unique_lock nav_lock(nav->mutex);
    std::unique_lock track_lock(track_nav->mutex);

    if (nav->value.smoothed_values_needs_update() ||
        track_nav->value.smoothed_values_needs_update()) {
        nav->value.update_smoothed_values(dt);
        track_nav->value.update_smoothed_values(dt);
    }
}


bool MainWindow::pointer_over_imgui() const {
    return ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                                  ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                  ImGuiHoveredFlags_AllowWhenBlockedByActiveItem) ||
           ImGui::IsAnyItemActive() ||
           ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
}


void MainWindow::draw_central() {
    // whatever the dock panels left over belongs to the piano roll or the track view
    EditorWidget& view = active_view();

    // no rect means the gl pass is skipped this frame
    view.context.work_rect = editor::ViewRect{0.0f, 0.0f, 0.0f, 0.0f};

    ImVec2 pos;
    ImVec2 size;
    if (!dock_manager->central_rect(pos, size)) {
        return;
    }

    view.context.work_rect =
        editor::ViewRect{pos.x, pos.y, std::max(1.0f, size.x), std::max(1.0f, size.y)};

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

    constexpr ImGuiWindowFlags window_flags =
        ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
        ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMouseInputs |
        ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoDocking;

    if (ImGui::Begin("##central", nullptr, window_flags)) {
        ImGui::Dummy(size);

        mouse_over_ui |= pointer_over_imgui();

        view.update();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

int MainWindow::run() {
    if (glfwInit() == GLFW_FALSE) {
        Debugger::log_error("Failed to initialise GLFW");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

    window_ = glfwCreateWindow(1920, 1080, "Andromeda", nullptr, nullptr);
    if (window_ == nullptr) {
        Debugger::log_error("Failed to create the window");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    glfwSetWindowUserPointer(window_, this);
    glfwSetDropCallback(window_, [](GLFWwindow* w, int count, const char** paths) {
        auto* self = static_cast<MainWindow*>(glfwGetWindowUserPointer(w));
        if (self == nullptr) {
            return;
        }

        std::vector<std::filesystem::path> dropped;
        dropped.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            dropped.emplace_back(paths[i]);
        }
        self->midi_io.on_files_dropped(std::move(dropped));
    });

    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
        Debugger::log_error("Failed to load OpenGL");
        glfwDestroyWindow(window_);
        glfwTerminate();
        return 1;
    }

    Debugger::log(std::format("OpenGL {}", reinterpret_cast<const char*>(glGetString(GL_VERSION))));
    {
        GLint depth_bits = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                              GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth_bits);
        Debugger::log(std::format("Depth buffer: {} bits", depth_bits));
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    app_scale_ = xscale;
    apply_egui_dark_theme(app_scale_);

    {
        ImGuiIO& io = ImGui::GetIO();
        const float font_size = EGUI_BODY_FONT_SIZE * app_scale_;

        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;

        if (io.Fonts->AddFontFromFileTTF("assets/fonts/Inter_18pt-Regular.ttf", font_size, &cfg,
                                         io.Fonts->GetGlyphRangesDefault()) != nullptr) {
            Debugger::log("UI font: assets/fonts/Inter_18pt-Regular.ttf");

            // do not merge a cjk font here: its metrics inflate the line height of all text
        } else {
            Debugger::log_warning(
                "assets/fonts/Ubuntu-Light.ttf missing; falling back to the system UI font.");
            io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", font_size, &cfg,
                                         io.Fonts->GetGlyphRangesKorean());
        }

        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    load_image_resources();
    init_dialogs();
    init_dock_panels();
    build_menu_bar();
    init_widgets();

    timer_.start();

    rendering::NoteGpuCache::set_enabled(startup_.gpu_notes);
    rendering::NoteOcclusionStore::set_enabled(startup_.note_cull);
    rendering::ScreenCoverage::set_enabled(startup_.coverage_cull);

    if (!startup_.file.empty()) {
        midi_io.on_files_dropped({startup_.file});
    }

    if (const char* debug_dialog = std::getenv("ANDROMEDA_DEBUG_DIALOG");
        debug_dialog != nullptr && *debug_dialog != '\0') {
        show_dialog(debug_dialog);
    }

    if (const char* debug_view = std::getenv("ANDROMEDA_DEBUG_VIEW");
        debug_view != nullptr && std::string_view(debug_view) == "track") {
        set_render_type(RenderType::TrackView);
    }

    if (startup_.bench_frames > 0) {
        glfwPollEvents();
        midi_io.handle_dropped_files();
        if (const auto import_status = midi_io.get_last_parse_status()) {
            on_midi_loaded(*import_status);
        }

        run_render_bench(startup_.bench_frames);

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
        return 0;
    }

    constexpr int SETTLE_FRAMES = 3;
    constexpr double IDLE_REDRAW_SECONDS = 0.25;
    int settle_frames = SETTLE_FRAMES;

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        if (settle_frames > 0 || wants_continuous_frames()) {
            glfwPollEvents();
        } else {
            const auto waited_from = std::chrono::steady_clock::now();
            glfwWaitEventsTimeout(IDLE_REDRAW_SECONDS);
            const double waited = std::chrono::duration<double>(
                                      std::chrono::steady_clock::now() - waited_from)
                                      .count();
            if (waited < IDLE_REDRAW_SECONDS * 0.9) {
                settle_frames = SETTLE_FRAMES;
            }
        }
        if (settle_frames > 0) {
            settle_frames -= 1;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        update_input_state();

        midi_io.handle_dropped_files();
        if (const auto import_status = midi_io.get_last_parse_status()) {
            on_midi_loaded(*import_status);
        }

        // TODO: move this in some dedicated update function
        {
            editor::MetaEditing* meta_editing = editor_controller.get_meta_editing();
            if (meta_editing) {
                bool recent_edit = meta_editing->has_edited_recently();
                if (recent_edit && bar_cacher) {
                    bar_cacher->clear_cache();
                }
            }
        }

        {
            // removed this since now editor classes use the project manager's ppq.
            /*std::unique_lock lock(project_manager->mutex);
            if (project_manager->value.ppq_changed) {
                const std::uint16_t ppq = project_manager->value.get_ppq();
                lock.unlock();
                update_global_ppq(ppq);
                lock.lock();
                project_manager->value.ppq_changed = false;
            }*/
        }

        static const bool frame_log = std::getenv("ANDROMEDA_FRAME_LOG") != nullptr;
        const auto frame_begin = std::chrono::steady_clock::now();

        mouse_over_ui = false;

        if (!has_crashed_) {
            try {
                handle_key_inputs();
                draw_ui();
                dialog_drawer.draw_all_dialogs(image_resources);
                process_closed_dialogs();
            } catch (const std::exception& e) {
                util::set_last_panic(e.what());
                Debugger::log_error(std::format("Frame aborted: {}", e.what()));
                has_crashed_ = true;
            } catch (...) {
                util::set_last_panic("Unknown panic");
                has_crashed_ = true;
            }
        } else {
            if (!crash_dlg_shown_) {
                Debugger::log_error("Oh no! Andromeda has crashed :(");
                show_dialog(dialog_names::DIALOG_NAME_CRASH);
                crash_dlg_shown_ = true;
            }

            dialog_drawer.draw_all_dialogs(image_resources);
        }

        ImGui::Render();

        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.04f, 0.04f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        const auto ui_done = std::chrono::steady_clock::now();

        draw_gl_surface();

        const auto gl_done = std::chrono::steady_clock::now();

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window_);

        if (frame_log) {
            const auto swapped = std::chrono::steady_clock::now();
            const auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };

            static double worst = 0.0;
            static double worst_ui = 0.0;
            static double worst_gl = 0.0;
            static double worst_swap = 0.0;
            static std::size_t worst_instances = 0;
            static std::size_t worst_cpu = 0;
            static int frames_this_second = 0;
            static auto second_start = frame_begin;

            const double total = ms(frame_begin, swapped);
            if (total > worst) {
                worst = total;
                worst_ui = ms(frame_begin, ui_done);
                worst_gl = ms(ui_done, gl_done);
                worst_swap = ms(gl_done, swapped);
                if (const auto* active = active_view().target_renderer.get()) {
                    worst_instances = active->instances_drawn();
                    worst_cpu = active->cpu_instances();
                }
            }
            frames_this_second += 1;

            if (ms(second_start, swapped) >= 1000.0) {
                Debugger::log(std::format(
                    "[frame] {} fps, worst {:.1f} ms (ui {:.1f}, notes {:.1f}, swap {:.1f}), "
                    "{} instances, {} of them packed by the CPU",
                    frames_this_second, worst, worst_ui, worst_gl, worst_swap, worst_instances,
                    worst_cpu));
                worst = 0.0;
                worst_instances = 0;
                worst_cpu = 0;
                frames_this_second = 0;
                second_start = swapped;
            }
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window_);
    glfwTerminate();

    return 0;
}

}
