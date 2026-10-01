#pragma once

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/editor_tool.h"
#include "app/main_window/midi_io.h"
#include "app/panel_layout.h"
#include "app/ui/dialog.h"
#include "app/ui/dialog_drawer.h"
#include "app/ui/dialog_manager.h"
#include "app/ui/dock_panels/dock_manager.h"
#include "app/rendering.h"
#include "app/rendering/data_view.h"
#include "app/rendering/note_cull_helper.h"
#include "app/rendering/note_uploader.h"
#include "app/shared.h"
#include "app/util/image_loader.h"
#include "app/view_settings.h"
#include "audio/audio_engine.h"
#include "util/system_stats.h"
#include "util/timer.h"
#include "audio/event_playback.h"
#include "audio/prerender_engine.h"
#include "editor/actions.h"
#include "editor/edit_functions.h"
#include "editor/editing.h"
#include "editor/editing/data_editing.h"
#include "editor/editing/meta_editing.h"
#include "editor/editing/note_editing.h"
#include "editor/editing/track_editing.h"
#include "editor/midi_bar_cacher.h"
#include "editor/navigation.h"
#include "editor/playhead.h"
#include "editor/plugins.h"
#include "editor/project/project_manager.h"
#include "midi/events/meta_event.h"
#include "util/shared.h"

#include <imgui.h>

struct GLFWwindow;

namespace andromeda::audio {
class MIDIDevices;
}

namespace andromeda::app {

class MainMenuBar;

using rendering::RenderType;

class MainWindow {
public:
    MainWindow();
    ~MainWindow();

    MainWindow(const MainWindow&) = delete;
    MainWindow& operator=(const MainWindow&) = delete;

    int run();

    struct StartupOptions {
        std::filesystem::path file;
        int bench_frames = 0;
        bool gpu_notes = true;
        bool note_cull = true;
        bool coverage_cull = false;
        bool bench_hide_data_view = false;
        bool bench_no_onion = false;
        float bench_play_at = 0.0f;
        float bench_zoom_ticks = 0.0f;
        float bench_zoom_keys = 0.0f;
        bool bench_still = false;
        std::string screenshot;
    };

    void set_startup_options(StartupOptions options) { startup_ = std::move(options); }

    void make_new_project();
    void save_project();
    void import_midi();
    void export_midi();

    void undo();
    void redo();
    [[nodiscard]] bool can_undo() const;
    [[nodiscard]] bool can_redo() const;

    [[nodiscard]] bool can_copy() const;
    [[nodiscard]] bool can_paste() const;
    [[nodiscard]] bool is_any_note_selected() const;

    void show_dialog(const char* name);
    void show_dialog_with_args(const char* name, DialogArgs args);

    void request_editing_copy();
    void request_editing_cut();
    void request_editing_paste();

    void apply_function(editor::EditFunction function_type);

    void insert_meta(midi::MetaEventType meta_type);

    void filter_selection_channels();

    void run_plugin(std::shared_ptr<editor::PluginLua> plugin);

    void curr_view_zoom_in_by(float x_fac, float y_fac);
    [[nodiscard]] std::optional<float> zoom_anchor_tick(float view_pos, float view_zoom) const;
    template <typename Nav>
    void zoom_ticks_around_anchor(Nav& n, float fac) const;

    void on_current_track_changed(std::uint16_t track);

    [[nodiscard]] std::uint16_t get_ppq() const;

    [[nodiscard]] std::pair<editor::MIDITick, editor::MIDITick> get_view_tick_range() const;

    [[nodiscard]] std::pair<editor::MIDITick, editor::MIDITick>
    get_view_tick_range_with_playback() const;

    void toggle_playback_from_start();
    void show_tick(editor::MIDITick tick);

    void switch_view(RenderType to);

    // extra render passes go here
    void render_data_view_pass();

    [[nodiscard]] bool wants_continuous_frames() const;

    [[nodiscard]] float get_playhead_pos(bool to_window) const;

    [[nodiscard]] float get_keyboard_width() const;

    [[nodiscard]] editor::MIDITick latest_note_start() const { return latest_note_start_; }

    [[nodiscard]] const std::string& status_text() const { return status_text_; }

    util::SharedPtr<editor::ProjectManager> project_manager;
    std::shared_ptr<editor::EditorActions> editor_actions;
    std::shared_ptr<editor::SharedClipboard> shared_clipboard;
    std::shared_ptr<editor::SharedSelectedNotes> shared_selected_notes;
    util::SharedPtr<editor::PianoRollNavigation> nav;
    util::SharedPtr<editor::TrackViewNavigation> track_nav;
    std::shared_ptr<editor::BarCacher> bar_cacher;

    std::shared_ptr<audio::AudioEngine> audio_engine;
    std::shared_ptr<audio::PlaybackManager> realtime_engine;
    std::shared_ptr<audio::PrerenderEngine> prerender_engine;
    std::shared_ptr<editor::Playhead> playhead = std::make_shared<editor::Playhead>();

    audio::SharedDevice midi_devices_slot;
    audio::SharedDevice kdmapi_slot;
    std::shared_ptr<audio::MIDIDevices> midi_devices;

    std::shared_ptr<EditorToolSettings> editor_tool_settings;
    std::shared_ptr<ToolBarSettings> toolbar_settings;
    std::shared_ptr<editor::NoteEditing> note_editing;
    std::shared_ptr<editor::DataEditing> data_editing;
    std::shared_ptr<editor::MetaEditing> meta_editing;
    std::shared_ptr<editor::TrackEditing> track_editing;
    std::shared_ptr<editor::EditFunctions> editor_functions =
        std::make_shared<editor::EditFunctions>();
    std::unique_ptr<editor::PluginLoader> plugin_loader;

    std::shared_ptr<DialogManager> dialog_manager = std::make_shared<DialogManager>();
    std::shared_ptr<DockManager> dock_manager = std::make_shared<DockManager>();
    DialogDrawer dialog_drawer;
    util::SharedMutPtr<ViewSettings> view_settings;
    NoteColorIndexing note_color_indexing = NoteColorIndexing::Channel;
    RenderType render_type = RenderType::PianoRoll;

    std::shared_ptr<NoteColors> note_colors;
    std::shared_ptr<rendering::NoteCullHelper> note_culler;
    std::shared_ptr<rendering::RenderManager> render_manager;

    rendering::NoteUploader note_uploader;

    std::shared_ptr<rendering::DataViewRenderer> data_view_renderer;
    editor::ViewRect data_view_rect;
    bool data_view_visible = false;

    // where the view was when the scroll bar was grabbed during playback, to return to
    std::optional<float> scroll_return_pos;

    void handle_data_view_inputs(bool pointer_in_panel);
    MIDIIoHandler midi_io;
    EventListenerHandler app_event_handler;

    void on_midi_loaded(midi::MIDIParseStatus import_status);

    void update_global_ppq(std::uint16_t ppq);
    void send_event_to_listeners(const AndromedaEvent& event);

    ImageResources image_resources;
    util::SystemStats sys_stats;

    bool mouse_over_ui = false;

private:
    void build_menu_bar();
    void load_image_resources();
    void init_render_manager();
    void init_dialogs();
    void init_dock_panels();
    void process_closed_dialogs();
    void draw_ui();
    void reset_ui_layout();
    void draw_central();

    void handle_trackview_editing_inputs(const editor::ViewRect& rect);

    [[nodiscard]] bool pointer_over_imgui() const;

    void draw_trackview_context_menu();

    void run_gl_pass(rendering::Renderer* renderer, float px, float py, float pw, float ph);
    void draw_gl_surface();

    void update_smoothed_values();

    void handle_key_inputs();

    void handle_pianoroll_navigation();
    void handle_trackview_navigation();

    void handle_editing_inputs(const editor::ViewRect& rect);
    void draw_select_box(const editor::ViewRect& rect);

    void draw_playhead_line(const editor::ViewRect& rect);

    void run_render_bench(int frames);

    StartupOptions startup_;

    util::Timer timer_;
    GLFWwindow* window_ = nullptr;
    // the area left for the piano roll / track view, set each frame from the dockspace
    ImVec2 central_pos_{};
    ImVec2 central_size_{};
    std::unique_ptr<MainMenuBar> menu_bar_;

    float app_scale_ = 1.0f;
    std::string status_text_;
    bool has_crashed_ = false;
    bool crash_dlg_shown_ = false;
    editor::MIDITick latest_note_start_ = 960 * 4 * 16;
};

}
