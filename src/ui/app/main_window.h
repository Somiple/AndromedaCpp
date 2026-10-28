#pragma once

// ---------------------------------------------------------------------------
// Layer: ui
// Dependency direction:  core -> midi -> editor -> audio -> ui
// The top layer: this is the only place allowed to include ImGui, GLFW and GL
// headers, and the only layer that may include every layer below it.
// See docs/ARCHITECTURE.md.
// ---------------------------------------------------------------------------

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// REFACTOR (include hygiene): only what this header genuinely needs is included
// here. The editing passes, the project manager and the plugin loader are all
// reachable through `editor/editor_controller.h` (which this header must include
// for the by-value `EditorController`), so they are not repeated; `main_window.cpp`
// includes them explicitly where it uses them directly.
#include "ui/app/app_options.h"
#include "ui/dialogs/dialog.h"
#include "ui/dialogs/dialog_drawer.h"
#include "ui/dialogs/dialog_manager.h"
#include "ui/layout/dock_manager.h"
#include "ui/rendering/rendering.h"
#include "ui/rendering/note_cull_helper.h"
#include "ui/rendering/note_uploader.h"
#include "ui/style/note_colors.h"
#include "ui/util/image_loader.h"
#include "ui/app/view_settings.h"
#include "ui/widgets/editor_widget.h"
#include "audio/midi_audio.h"
#include "core/system_stats.h"
#include "core/timer.h"
#include "editor/edit_functions.h"
#include "editor/navigation.h"
#include "editor/plugins.h"
#include "midi/events/meta_event.h"
#include "core/shared.h"
#include "core/math/vector2.h"
#include "editor/editor_controller.h"

#include <imgui.h>

struct GLFWwindow;

namespace andromeda::audio {
class MIDIDevices;
}

namespace andromeda::app {

class MainMenuBar;
class PianoRollWidget;
class TrackViewWidget;
class DataViewWidget;
class EditorApp;
// refactor: project-session flows moved out of MainWindow into their own coordinator.
class ProjectActions;
// refactor: view activation and the active-track fact moved out of MainWindow.
class ViewCoordinator;
// refactor: dialog opening and closed-dialog routing moved out of MainWindow.
class DialogCoordinator;

using rendering::RenderType;
using andromeda::editor::history::ActionHistory;

// REFACTOR: single canonical definition now lives in editor/editor_context.h. Re-exported
// here so every existing `app::KeyModifierState` spelling keeps compiling, and so the type
// the window fills each frame is the very one EditorContext points at.
using editor::KeyModifierState;

/// The application window: GLFW/ImGui/GL surface, docking, dialogs and input.
///
/// Subsystems are reached through the accessor blocks below instead of through
/// public data members, so a panel or widget depends on the window's *interface*
/// rather than on where (or how) a subsystem happens to be stored.
///
/// REFACTOR: the data members that used to be public now live in the private
/// "subsystem storage" region at the bottom of this class. Each one has an
/// accessor; call sites use `mw.<accessor>()` instead of `mw.<member>`.
class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    int run();

    /// Launch-time configuration; the nested name is kept as an alias of `AppOptions`
    /// so existing call sites (`MainWindow::StartupOptions`) keep compiling.
    using StartupOptions = AppOptions;

    void set_startup_options(StartupOptions options) { startup_ = std::move(options); }

#pragma region Frame / input
    /// Mouse position in viewport-normalised coordinates for the current frame.
    [[nodiscard]] const util::math::Vector2<float>* get_mouse_pos() const { return &_mouse_pos; }
    /// Whether shift / ctrl / alt were held this frame.
    [[nodiscard]] const KeyModifierState* get_key_modifier_state() const { return &_key_modifiers; }
    /// Tick range of the central widget currently on screen.
    [[nodiscard]] TickView get_central_tick_view() const { return active_view().true_tick_view(); }

    /// True while the pointer is over any ImGui window this frame.
    [[nodiscard]] bool is_mouse_over_ui() const noexcept { return mouse_over_ui_; }
    /// Sets the "pointer is over UI" flag for this frame.
    void set_mouse_over_ui(bool value) noexcept { mouse_over_ui_ = value; }

    /// Narrow, editor-owned context handed to editor components; refreshed once per frame.
    [[nodiscard]] editor::EditorContext& editor_context() noexcept { return _editor_context; }
    /// Const view of the same context.
    [[nodiscard]] const editor::EditorContext& editor_context() const noexcept { return _editor_context; }
#pragma endregion

#pragma region Subsystem accessors
    /// Document model, editing passes, selection and undo history.
    [[nodiscard]] editor::EditorController& controller() noexcept { return editor_controller; }
    /// Const view of the editor controller.
    [[nodiscard]] const editor::EditorController& controller() const noexcept { return editor_controller; }

    /// Piano-roll viewport state (scroll position and zoom); guarded by its own mutex.
    [[nodiscard]] util::SharedPtr<editor::PianoRollNavigation>& navigation() noexcept { return nav; }
    /// Const view of the piano-roll viewport state.
    [[nodiscard]] const util::SharedPtr<editor::PianoRollNavigation>& navigation() const noexcept { return nav; }

    /// Track-view viewport state; independent of `navigation()`.
    [[nodiscard]] util::SharedPtr<editor::TrackViewNavigation>& track_navigation() noexcept { return track_nav; }
    /// Const view of the track-view viewport state.
    [[nodiscard]] const util::SharedPtr<editor::TrackViewNavigation>& track_navigation() const noexcept { return track_nav; }

    /// The value inside the mutex-guarded view settings (read without locking).
    [[nodiscard]] ViewSettings& settings() noexcept { return view_settings->value; }
    /// Shared, mutex-guarded view settings; lock explicitly before mutating.
    [[nodiscard]] util::SharedMutPtr<ViewSettings>& view_settings_handle() noexcept { return view_settings; }

    /// Script / Andromeda object plugins loaded at startup.
    [[nodiscard]] std::unique_ptr<editor::PluginLoader>& plugins() noexcept { return plugin_loader; }

    /// Dialog stack and the factories registered by `init_dialogs()`.
    [[nodiscard]] DialogManager& dialogs() noexcept { return *dialog_manager; }
    /// Dock layout owner: panel groups and layout resets.
    [[nodiscard]] DockManager& docks() noexcept { return *dock_manager; }
    /// Draws the currently open dialog stack inside the running ImGui frame.
    [[nodiscard]] DialogDrawer& dialog_drawer() noexcept { return dialog_drawer_; }

    /// Note colour palette plus its generated GPU texture.
    [[nodiscard]] NoteColors& colors() noexcept { return *note_colors; }
    /// How notes are coloured (by track, by channel, or both).
    [[nodiscard]] NoteColorIndexing note_color_indexing() const noexcept { return note_color_indexing_; }
    /// Changes the note colour-indexing mode; takes effect on the next frame.
    void set_note_color_indexing(NoteColorIndexing mode) noexcept { note_color_indexing_ = mode; }

    /// Note-visibility culling helper bound to the project's track list.
    [[nodiscard]] std::shared_ptr<rendering::NoteCullHelper>& note_culler() noexcept { return note_culler_; }

    /// The editor widgets: piano roll, track view and data view.
    [[nodiscard]] std::vector<std::unique_ptr<EditorWidget>>& views() noexcept { return widgets; }
    /// Background worker that uploads note data into the GPU caches.
    [[nodiscard]] rendering::NoteUploader& uploader() noexcept { return note_uploader; }

    /// View position to restore after a playback drag of the scrollbar.
    [[nodiscard]] std::optional<float>& scroll_return_pos() noexcept { return scroll_return_pos_; }

    // refactor: the MIDIIoHandler accessor was deleted — midi import/export now belongs to
    // ProjectActions (project_actions().import_midi() / export_midi()) and the headless
    // file work to midi/io/midi_file_io.h.
    /// Registered application event listeners (PPQ and editor-tool changes).
    [[nodiscard]] EventListenerHandler& events() noexcept { return app_event_handler; }

    /// Project-session flows: new project, save, and the dialog routing that drives them.
    [[nodiscard]] ProjectActions& project_actions() noexcept { return *project_actions_; }

    /// Preloaded toolbar and icon textures.
    [[nodiscard]] ImageResources& images() noexcept { return image_resources; }
    /// CPU / memory counters refreshed by the diagnostics panel.
    [[nodiscard]] util::SystemStats& stats() noexcept { return sys_stats; }
#pragma endregion

#pragma region Scene state
    // refactor: activation is owned by ViewCoordinator::set_render_type; the window only
    // forwards, so switch_view and set_render_type share one activation path.
    /// Which central renderer is active (piano roll or track view).
    [[nodiscard]] RenderType render_type() const noexcept { return render_type_; }
    /// Switches the central renderer; carries the view position over.
    void set_render_type(RenderType to);
#pragma endregion

    [[nodiscard]] EditorApp* get_editor_app() { return _app.get(); }
    [[nodiscard]] const EditorApp* get_editor_app() const { return _app.get(); }

    // refactor: make_new_project/save_project/import_midi/export_midi were all removed from
    // MainWindow — project-session and midi flows now belong to ui/app/project_actions.h
    // (reached through project_actions()).

    void show_dialog(const char* name);
    void show_dialog_with_args(const char* name, DialogArgs args);

    void apply_function(editor::EditFunction function_type);

    void insert_meta(midi::MetaEventType meta_type);

    void filter_selection_channels();

    void run_plugin(std::shared_ptr<editor::PluginLua> plugin);

    void curr_view_zoom_in_by(float x_fac, float y_fac);
    [[nodiscard]] std::optional<float> zoom_anchor_tick(float view_pos, float view_zoom) const;
    [[nodiscard]] std::optional<float> mouse_tick(float view_pos, float view_zoom) const;

    // refactor: forwards to ViewCoordinator; the selected track lives on EditorController.
    void on_current_track_changed(std::uint16_t track);

    [[nodiscard]] std::uint16_t get_ppq();

    [[nodiscard]] std::pair<editor::MIDITick, editor::MIDITick> get_view_tick_range() const;

    [[nodiscard]] std::pair<editor::MIDITick, editor::MIDITick> get_view_tick_range_with_playback() const;

    void toggle_playback_from_start();
    void show_tick(editor::MIDITick tick);

    // refactor: forwards to ViewCoordinator::switch_view, which delegates activation to
    // set_render_type and activates the data-view renderer there.
    void switch_view(RenderType to);

    [[nodiscard]] bool wants_continuous_frames() const;

    // REFACTOR: the deprecated `get_playhead_pos(bool)` stub was removed - it had no
    // callers and its body was already commented out.
    [[nodiscard]] float get_keyboard_width() const;

    [[nodiscard]] float app_scale() const { return app_scale_; }

    // empty while the window has no size, e.g. minimised
    [[nodiscard]] std::optional<GlSurface> gl_surface() const;

    // the central widget on screen: the piano roll or the track view
    [[nodiscard]] EditorWidget& active_view() const;
    [[nodiscard]] EditorWidget& data_view() const;

    [[nodiscard]] editor::MIDITick latest_note_start() const { return latest_note_start_; }

    [[nodiscard]] const std::string& status_text() const { return status_text_; }

    void on_midi_loaded(midi::MIDIParseStatus import_status);

    void update_global_ppq(std::uint16_t ppq);
    void send_event_to_listeners(const AndromedaEvent& event);

private:
    struct PendingRefresh {
        bool global_metas = false;
    };
    PendingRefresh pending_{};

    void build_menu_bar();
    void load_image_resources();
    void init_widgets();
    void init_dialogs();
    void init_dock_panels();
    void process_closed_dialogs();
    void draw_ui();
    void reset_ui_layout();
    void draw_central();

    [[nodiscard]] bool pointer_over_imgui() const;

    void draw_gl_surface();
    void flush_pending_refresh();
    void update_smoothed_values();

    void handle_key_inputs();

    void update_input_state();
    void run_render_bench(int frames);

    StartupOptions startup_;

    util::Timer timer_;
    GLFWwindow* window_ = nullptr;
    std::unique_ptr<MainMenuBar> menu_bar_;

    // the ACTUAL APP ITSELF
    std::unique_ptr<EditorApp> _app;

    // owned by widgets
    PianoRollWidget* piano_roll_widget_ = nullptr;
    TrackViewWidget* track_view_widget_ = nullptr;
    DataViewWidget* data_view_widget_ = nullptr;

    // a left press that started inside the view and wasn't claimed by ui; the view keeps owning
    // it until release, even if imgui starts moving the dockspace window under it
    bool view_press_ = false;

    float app_scale_ = 1.0f;
    std::string status_text_;
    bool has_crashed_ = false;
    bool crash_dlg_shown_ = false;
    editor::MIDITick latest_note_start_ = 960 * 4 * 16;

    // input stuff
    util::math::Vector2<float> _mouse_pos{};
    KeyModifierState _key_modifiers{};

#pragma region Subsystem storage (reached through the accessors above)
    // Storage keeps the names it had as a public member wherever that name does
    // not clash with an accessor, so the window's own implementation reads the
    // same as before. Names that would clash gain a trailing underscore.
    editor::EditorController editor_controller;

    // navigation
    util::SharedPtr<editor::PianoRollNavigation> nav;
    util::SharedPtr<editor::TrackViewNavigation> track_nav;

    std::unique_ptr<editor::PluginLoader> plugin_loader;

    std::shared_ptr<DialogManager> dialog_manager = std::make_shared<DialogManager>();
    std::shared_ptr<DockManager> dock_manager = std::make_shared<DockManager>();
    // clashes with the dialog_drawer() accessor, hence the trailing underscore
    DialogDrawer dialog_drawer_;
    util::SharedMutPtr<ViewSettings> view_settings;

    // NOTE: these four clash with their accessor names, hence the trailing underscore.
    NoteColorIndexing note_color_indexing_ = NoteColorIndexing::Channel;
    RenderType render_type_ = RenderType::PianoRoll;
    std::shared_ptr<rendering::NoteCullHelper> note_culler_;
    std::optional<float> scroll_return_pos_;
    bool mouse_over_ui_ = false;

    // REFACTOR: narrow context handed to editor components; refreshed once per frame by
    // update_input_state(), so components never touch MainWindow.
    editor::EditorContext _editor_context;

    std::shared_ptr<NoteColors> note_colors;
    // piano roll, track view, data view
    std::vector<std::unique_ptr<EditorWidget>> widgets;

    // refactor: project-session owner (new/save). trailing underscore because the
    // project_actions() accessor above already uses the plain name, as with the other
    // clashing storage members in this region.
    std::unique_ptr<ProjectActions> project_actions_;

    // refactor: view activation + active track; the window only forwards to it, so this
    // replaces the bodies of switch_view and on_current_track_changed.
    std::unique_ptr<ViewCoordinator> view_coordinator_;

    // refactor: dialog open/close policy; the factory table stays in init_dialogs().
    std::unique_ptr<DialogCoordinator> dialog_coordinator_;

    rendering::NoteUploader note_uploader;

    EventListenerHandler app_event_handler;

    ImageResources image_resources;
    util::SystemStats sys_stats;
#pragma endregion
};

}
