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
#include "main_window_internal.h"
#include "editor_context_bridge.h"
// refactor: project-session flows now live in this coordinator instead of MainWindow.
#include "ui/app/project_actions.h"
// refactor: view activation + active-track owner, likewise out of MainWindow.
#include "ui/app/view_coordinator.h"
// refactor: dialog opening and closed-dialog routing, likewise out of MainWindow.
#include "ui/app/dialog_coordinator.h"
// refactor: one generic parameters dialog now renders any command's declared inputs.
#include "ui/dialogs/edit_function_params_dialog.h"
// refactor: projects the edit-function registry into the menu bar.
#include "ui/app/edit_function_menu.h"

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

namespace andromeda::app {

using util::Debugger;

namespace {

using managers::PlaybackManager;

} // namespace

// REFACTOR: row heights moved to main_window_internal.h. The dock wiring now lives in its
// own translation unit and needs the same values, so a second anonymous-namespace copy
// would have silently drifted. The unqualified names used below are unchanged.
using internal::TOOLBAR_ROW_H;
using internal::TRANSPORT_ROW_H;
using internal::SLIDER_ROW_H;
using internal::BAR_NUMBER_ROW_H;

MainWindow::MainWindow()
    : editor_controller(this),
      nav(util::make_shared_rw<editor::PianoRollNavigation>()),
      track_nav(util::make_shared_rw<editor::TrackViewNavigation>()),
      view_settings(util::make_shared_mut<ViewSettings>()) {

    editor::ProjectManager* project_manager = editor_controller.get_project_manager();
    project_manager->new_empty_project();

    // REFACTOR: editing operations that need a dialog (insert meta, filter channels)
    // moved into the controller; it borrows the window's dialog manager instead of
    // reaching back through MainWindow.
    editor_controller.set_dialog_manager(dialog_manager.get());

    // refactor: project-session operations (new project, save) moved out of MainWindow
    // into ProjectActions. the window only wires the coordinator up: it hands over the
    // project manager, a dialog opener and the post-swap reset, and keeps no policy here.
    project_actions_ = std::make_unique<ProjectActions>(ProjectActionsContext{
        .project = *project_manager,
        .open_dialog = [this](const char* name, DialogArgs args) {
            show_dialog_with_args(name, std::move(args));
        },
        .on_project_reset = [this] {
            editor_controller.get_history()->clear();
            _app->get_playback_manager()->reset_playback();
            std::unique_lock lock(nav->mutex);
            nav->value.tick_pos = 0.0f;
        },
        .set_status = [this](const std::string& text) { status_text_ = text; },
    });

    // refactor: view activation and the active-track fact moved to ViewCoordinator. it gets
    // the navigation state, the render-type storage and the widget pointers by reference —
    // the widgets are still null here and are assigned later by init_widgets().
    view_coordinator_ = std::make_unique<ViewCoordinator>(ViewCoordinatorServices{
        .navigation = nav,
        .track_navigation = track_nav,
        .controller = editor_controller,
        .active_type = render_type_,
    });

    // refactor: dialog open/close policy moved to DialogCoordinator; only a confirmed id is
    // routed back, and ProjectActions stays the owner of that decision.
    dialog_coordinator_ = std::make_unique<DialogCoordinator>(DialogCoordinatorContext{
        .manager = *dialog_manager,
        .handle_confirmed_id = [this](std::string_view dialog_id) {
            return project_actions_->handle_confirmed_dialog(dialog_id);
        },
        // refactor: a confirmed parameters dialog hands its values straight to the registry,
        // which owns the command body.
        .handle_edit_function_args = [this](std::string_view function_id,
                                            const editor::EditFunctionArgs& values) {
            return editor_controller.edit_functions().invoke(function_id, values,
                                                             editor_controller);
        },
    });

    // refactor: the menu is a projection of the registry now; the window hands over the
    // registry, the controller and one callback, and keeps no command knowledge itself.
    edit_function_menu_ = std::make_unique<EditFunctionMenu>(EditFunctionMenuContext{
        .registry = editor_controller.edit_functions(),
        .controller = editor_controller,
        .open_params_dialog = [this](std::string_view function_id) {
            // refactor: the registry supplies the label and the declared parameters, so this
            // no longer names a dialog per command - adding one needs no change here.
            const editor::EditFunctionRegistry& registry = editor_controller.edit_functions();
            const editor::EditFunctionInfo* info = registry.find(function_id);

            DialogArgs args;
            args.emplace_back(EditFunctionDialogArgs{
                std::string(function_id),
                info != nullptr ? info->label : std::string(function_id),
                registry.params_of(function_id),
                registry.default_args(function_id),
            });
            show_dialog_with_args(dialog_names::DIALOG_NAME_EDIT_FUNCTION_PARAMS,
                                  std::move(args));
        },
    });

    plugin_loader = std::make_unique<editor::PluginLoader>("assets/plugins/custom",
                                                           "assets/plugins/builtin");
    if (auto result = plugin_loader->load_all_plugins(); !result) {
        Debugger::log_warning(std::format("Custom plugins were not loaded: {}", result.error()));
    }

    editor_controller.get_executor()->set_observer([this](const editor::history::ChangeEvent& e) {
        using editor::history::ChangeKind;
        switch (e.kind) {
        case ChangeKind::GlobalMetaEvents: pending_.global_metas = true; break;
        }
    });

    _app = std::make_unique<EditorApp>(this);
}

MainWindow::~MainWindow() = default;

// refactor: make_new_project(), save_project(), import_midi() and export_midi() were all
// removed from MainWindow. their bodies now live in ui/app/project_actions.cpp, which owns
// project-session and midi policy; the window keeps no project logic and no midi handler.
void MainWindow::on_midi_loaded(midi::MIDIParseStatus import_status) {
    switch (import_status) {
    case midi::MIDIParseStatus::ParseOK: {
        editor::ProjectManager* manager = editor_controller.get_project_manager();
        {
            update_global_ppq(manager->get_ppq());
        }

        _app->get_playback_manager()->reset_playback();
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

        // reset editor controller to a clean state
        editor_controller.cleanup_editors();
        // its ok to explicitly call this, the history gets cleared anyways
        editor_controller.get_selection()->clear_selected();

        editor_controller.get_history()->clear();

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
    // refactor: the body moved to DialogCoordinator::open; the window only forwards.
    dialog_coordinator_->open(name, std::move(args));
}

void MainWindow::init_dialogs() {
    using namespace dialog_names;

    dialog_drawer_.init(dialog_manager);
    dialog_drawer_.on_terminate_app = [this]() {
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


    // refactor: one factory covers every parameterised command; the registry supplies the
    // label and the declared parameter list through DialogArgs.
    dialog_manager->register_dialog(DIALOG_NAME_EDIT_FUNCTION_PARAMS, []() {
        return std::make_unique<EditFunctionParamsDialog>();
    });

    dialog_manager->register_dialog(DIALOG_NAME_PROJECT_SETTINGS, [this]() {
        return std::make_unique<editor::ProjectSettings>(editor_controller.get_project_manager());
    });

    dialog_manager->register_dialog(DIALOG_NAME_EDITOR_SETTINGS, [this]() {
        auto settings = std::make_unique<editor::ESSettingsWindow>();
        settings->set_audio_engine(_app->get_audio_engine());
        return settings;
    });

    dialog_manager->register_dialog(DIALOG_NAME_PLUGIN_DIALOG, [this]() {
        auto plugin_dialog = std::make_unique<editor::PluginDialog>();
        plugin_dialog->init(&editor_controller);
        return plugin_dialog;
    });
}

void MainWindow::process_closed_dialogs() {
    // refactor: the body moved to DialogCoordinator::process_closed; the window only forwards.
    dialog_coordinator_->process_closed();
}

// refactor: the enum and the holds_alternative dialog dispatch are gone. the registry decides
// whether a command needs parameters, so these two overloads are the whole UI entry point.
void MainWindow::apply_function(std::string_view function_id) {
    editor_controller.perform_function(function_id);
}

void MainWindow::apply_function(std::string_view function_id,
                                const editor::EditFunctionArgs& args) {
    editor_controller.perform_function(function_id, args);
}

// REFACTOR: forwarding stub - the real work lives in EditorController::insert_meta
void MainWindow::insert_meta(midi::MetaEventType meta_type) {
    // The window only supplies what it owns: the playhead position.
    const midi::MIDITick playhead_tick = _app->get_playback_manager()->get_playback_start_ticks();
    editor_controller.insert_meta(meta_type, playhead_tick);
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
        std::make_shared<editor::AndromedaObj>(editor_controller.get_project_manager(), _app->get_playback_manager());

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
    const auto* playback_manager = _app->get_playback_manager();
    const bool playing = playback_manager->is_playing();

    bool autoscroll = false;
    {
        std::lock_guard lock(view_settings->mutex);
        autoscroll = view_settings->value.pr_autoscroll;
    }

    // the view is free (stopped, or playing without autoscroll): zoom around the mouse
    const bool free_view = !playing || !autoscroll;
    if (free_view) {
        if (const auto tick = mouse_tick(view_pos, view_zoom)) {
            return tick;
        }
    }

    const auto* timeline = editor_controller.get_project_manager()->get_timeline();
    const auto* timer = _app->get_timer();
    const float anchor = static_cast<float>(playing && !autoscroll
        ? timeline->secs_to_tick(timer->elapsed())
        : playback_manager->get_playback_start_ticks());
    if (anchor >= view_pos && anchor <= view_pos + view_zoom) {
        return anchor;
    }

    // nothing sensible to hold still (mouse off the notes, playhead/seek point off screen).
    // returning nothing here made the zoom keep the left edge, so use the view centre instead
    if (free_view) {
        return view_pos + view_zoom * 0.5f;
    }
    return std::nullopt;
}

// the tick under the mouse horizontally, or nothing if the mouse is outside the notes' x range.
// only x matters for a tick, so hovering the ruler above or a panel below still counts
std::optional<float> MainWindow::mouse_tick(float view_pos, float view_zoom) const {
    auto rect = active_view().get_component()->get_working_rect();   // screen-space {x, y, w, h}, excluding the keyboard
    if (rect.width <= 0.0f) return std::nullopt;

    const float mx = _mouse_pos.x;
    if (mx < rect.left || mx > rect.left + rect.width) {
        return std::nullopt;
    }

    return view_pos + (mx - rect.left) / rect.width * view_zoom;
}

EditorWidget& MainWindow::active_view() const {
    if (render_type_ == RenderType::TrackView) {
        return *track_view_widget_;
    }
    return *piano_roll_widget_;
}

EditorWidget& MainWindow::data_view() const { return *data_view_widget_; }

void MainWindow::curr_view_zoom_in_by(float x_fac, float y_fac) {
    active_view().zoom_by(x_fac, y_fac);
}

// refactor: the body moved to ViewCoordinator::on_current_track_changed; the window only
// forwards so existing call sites keep their spelling.
void MainWindow::on_current_track_changed(std::uint16_t track) {
    view_coordinator_->on_current_track_changed(track);
}

// refactor: the query itself moved to ProjectActions::ppq(); the window only forwards so
// existing `mw.get_ppq()` call sites keep their spelling.
std::uint16_t MainWindow::get_ppq() {
    return project_actions_->ppq();
}

float MainWindow::get_keyboard_width() const { return active_view().keyboard_width(); }

// stopping returns the playhead to where playback started; autoscroll never moved the view
// itself, so leaving it alone brings it back there too
void MainWindow::toggle_playback_from_start() {
    PlaybackManager* playback_manager = _app->get_playback_manager();
    if (!playback_manager->is_playing()) {
        // fixed rust bug: playing with the playhead scrolled out of view kept the view away
        show_tick(playback_manager->get_playback_start_ticks());
        playback_manager->start_playback();
        return;
    } else {
        playback_manager->stop_playback();
    }

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
    if (_app->get_playback_manager()->is_playing()) return true;

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

// refactor: both bodies moved to ViewCoordinator::switch_view / set_render_type; the window
// forwards so existing call sites keep their spelling.
void MainWindow::switch_view(RenderType to) { view_coordinator_->switch_view(to); }

void MainWindow::set_render_type(RenderType to) { view_coordinator_->set_render_type(to); }

std::pair<editor::MIDITick, editor::MIDITick> MainWindow::get_view_tick_range_with_playback() const {
    const auto* playback_manager = _app->get_playback_manager();
    const bool is_playing = playback_manager->is_playing();

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
    const auto* timeline = editor_controller.get_project_manager()->get_timeline();
    const float playback = static_cast<float>(timeline->secs_to_tick(_app->get_timer()->elapsed()));
    const float start = static_cast<float>(playback_manager->get_playback_start_ticks());
    const float min = std::max(0.0f, playback + pos - start);
    return {static_cast<editor::MIDITick>(min), static_cast<editor::MIDITick>(min + zoom)};
}

// REFACTOR: the deprecated `get_playhead_pos(bool)` stub was removed from here.
// It had no callers, and its body was already commented out.

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
    note_culler_ = std::make_shared<rendering::NoteCullHelper>(manager->get_tracks());

    auto piano_roll = std::make_unique<PianoRollWidget>(*this);
    auto track_view = std::make_unique<TrackViewWidget>(*this);
    const std::shared_ptr<rendering::NoteGpuCache> note_cache = piano_roll->renderer().note_cache();
    auto data_view = std::make_unique<DataViewWidget>(*this, note_cache);

    piano_roll_widget_ = piano_roll.get();
    track_view_widget_ = track_view.get();
    data_view_widget_ = data_view.get();

    // refactor: the widgets only exist from here on, so the view coordinator is told about
    // them before the first set_render_type() below.
    view_coordinator_->set_widgets(piano_roll_widget_, track_view_widget_, data_view_widget_);

    widgets.push_back(std::move(piano_roll));
    widgets.push_back(std::move(track_view));
    widgets.push_back(std::move(data_view));

    set_render_type(RenderType::PianoRoll);

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

    // REFACTOR: refresh the narrow context handed to editor components. The pointers stay
    // valid for the whole frame because the members they address live as long as the window.
    _editor_context.mouse_pos = &_mouse_pos;
    _editor_context.key_modifiers = &_key_modifiers;
    _editor_context.mouse_over_ui = &mouse_over_ui_;
    _editor_context.central_tick_view = get_central_tick_view();
    _editor_context.nav = &nav;
    _editor_context.track_nav = &track_nav;
    _editor_context.playback = _app != nullptr ? _app->get_playback_provider() : nullptr;
    // refactor: the controller carries the same transport the context hands to components, so
    // an edit function reads the playhead through EditorController without reaching for a
    // window or app pointer. kept in lockstep with the line above so the two never disagree,
    // and so a recreated EditorApp cannot leave a dangling pointer behind.
    editor_controller.set_playback_provider(_editor_context.playback);
    // One small struct assignment per frame; keeps the editor layer free of ViewSettings.
    sync_view_snapshot(_editor_context, view_settings->value, editor_controller.get_active_track());
}

std::optional<GlSurface> MainWindow::gl_surface() const {
    int fb_w = 0;
    int fb_h = 0;
    glfwGetFramebufferSize(window_, &fb_w, &fb_h);
    
    const ImVec2 display = ImGui::GetIO().DisplaySize;
    if (display.x <= 0.0f || display.y <= 0.0f) {
        return std::nullopt;
    }
    
    return GlSurface{ fb_w, fb_h, static_cast<float>(fb_w) / display.x,
                     static_cast<float>(fb_h) / display.y };
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

void MainWindow::flush_pending_refresh() {
    auto project_manager = editor_controller.get_project_manager();
    auto p = std::exchange(pending_, {});

    if (p.global_metas) {
        project_manager->get_timeline()->rebuild(
            project_manager->get_project_data().global_metas, project_manager->get_ppq());
    }
}

void MainWindow::handle_key_inputs() {
    if (widgets.empty()) {
        return;
    }

    if (ImGui::GetIO().WantCaptureKeyboard) {
        return;
    }

    if (ImGui::IsKeyPressed(ImGuiKey_Tab, false)) {
        switch (render_type_) {
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
    if (ImGui::IsAnyItemActive() ||
        ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel)) {
        return true;
    }

    if (!ImGui::IsWindowHovered(ImGuiHoveredFlags_AnyWindow |
                                ImGuiHoveredFlags_AllowWhenBlockedByPopup |
                                ImGuiHoveredFlags_AllowWhenBlockedByActiveItem)) {
        return false;
    }

    const ImGuiWindow* hovered = ImGui::GetCurrentContext()->HoveredWindow;
    if (hovered == nullptr) {
        return false;
    }

    // imgui pads every window's hit-test rect by a few px (resize borders / splitters). Those
    // pixels belong to the view when they fall inside it.
    const ImVec2 mp = ImGui::GetIO().MousePos;
    const editor::ViewRect& view = active_view().context.work_rect;
    const bool inside_view = mp.x >= view.left && mp.x < view.left + view.width &&
                             mp.y >= view.top && mp.y < view.top + view.height;

    if (inside_view) {
        // Only a genuinely floating window can sit on top of the view. Docked panels are laid
        // out around it, and the dockspace host window spans the whole viewport (it only stops
        // being hovered over the view through a hit-test hole that lags a frame behind layout
        // changes, which is why this used to work only some of the time).
        // the dockspace lives in a child window of the host, so walk the whole parent chain
        bool is_dock_host = false;
        bool is_docked = false;
        for (const ImGuiWindow* w = hovered; w != nullptr; w = w->ParentWindow) {
            is_dock_host |= w->DockNodeAsHost != nullptr;
            is_docked |= w->DockNode != nullptr;
        }
        const ImGuiWindow* root = hovered->RootWindow != nullptr ? hovered->RootWindow : hovered;
        return !is_dock_host && !is_docked && root->Rect().Contains(mp);
    }

    return true;
}


void MainWindow::draw_central() {
    // whatever the dock panels left over belongs to the piano roll or the track view
    EditorWidget& view = active_view();

    // no rect means the gl pass is skipped this frame
    view.context.work_rect = editor::ViewRect{ 0.0f, 0.0f, 0.0f, 0.0f };

    ImVec2 pos;
    ImVec2 size;
    if (!dock_manager->central_rect(pos, size)) {
        return;
    }

    view.context.work_rect =
        editor::ViewRect{ pos.x, pos.y, std::max(1.0f, size.x), std::max(1.0f, size.y) };

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

        const bool over_imgui = pointer_over_imgui();
        {
            const ImVec2 mp = ImGui::GetIO().MousePos;
            const bool in_view = mp.x >= pos.x && mp.x < pos.x + size.x &&
                                 mp.y >= pos.y && mp.y < pos.y + size.y;
            if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) ||
                ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
                view_press_ = in_view && !mouse_over_ui_ && !over_imgui;
            }
        }
        const bool press_latched =
            view_press_ && (ImGui::IsMouseDown(ImGuiMouseButton_Left) ||
                            ImGui::IsMouseReleased(ImGuiMouseButton_Left) ||
                            ImGui::IsMouseDown(ImGuiMouseButton_Right) ||
                            ImGui::IsMouseReleased(ImGuiMouseButton_Right));

        if (!press_latched) {
            view_press_ = false;
            mouse_over_ui_ |= over_imgui;
        }

        view.update();
    }

    ImGui::End();
    ImGui::PopStyleVar();
}

}