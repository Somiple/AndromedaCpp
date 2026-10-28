// REFACTOR: menu construction extracted verbatim from main_window.cpp (lines 748-936).
// The menu is pure UI description; it does not touch window lifecycle state.
// Behaviour unchanged.
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
// refactor: menu entries below route project-session actions to ProjectActions instead of
// calling (now removed) MainWindow::make_new_project / save_project.
#include "ui/app/project_actions.h"
// refactor: the Tools -> Editing submenu is projected from the edit-function registry, so the
// complete type is needed here (main_window.h only forward-declares it).
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

// REFACTOR: same alias main_window.cpp used before the split.
using util::Debugger;

void MainWindow::build_menu_bar() {
    menu_bar_ = std::make_unique<MainMenuBar>();

    if (const ImageHandle logo = image_resources.get_image_handle("logo_small"); logo.valid()) {
        menu_bar_->add_menu_image_action(logo.texture_id, 16.0f, 16.0f,
                                         [](MainWindow& mw) { mw.show_dialog(dialog_names::DIALOG_NAME_EDITOR_INFO); });
    }

    menu_bar_->add_menu("File", {
        {"New Project", MenuButton{[](MainWindow& mw) { mw.project_actions().request_new_project(); }}},
        {"Save Project (WIP)", MenuButton{[](MainWindow& mw) { mw.project_actions().save_project(); }}},
        {"", MenuSeparator{}},
        {"Import MIDI file", MenuButton{[](MainWindow& mw) { mw.project_actions().import_midi(); }}},
        {"Export MIDI file", MenuButton{[](MainWindow& mw) { mw.project_actions().export_midi(); }}},
    });

    const auto note_selected = [](MainWindow& mw) { return mw.controller().get_selection()->is_any_note_selected(); };

    menu_bar_->add_menu("Edit", {
        {"Undo", MenuButtonEnabled{[](MainWindow& mw) { mw.controller().undo(); },
                                   [](MainWindow& mw) { return mw.controller().can_undo(); }}},
        {"Redo", MenuButtonEnabled{[](MainWindow& mw) { mw.controller().redo(); },
                                   [](MainWindow& mw) { return mw.controller().can_redo(); }}},
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
        {"Copy", MenuButtonEnabled{[](MainWindow& mw) { mw.controller().copy(); },
                                   [](MainWindow& mw) { return mw.controller().can_copy(); }}},
        {"Cut", MenuButtonEnabled{[](MainWindow& mw) { mw.controller().cut(); },
                                  [](MainWindow& mw) { return mw.controller().can_copy(); }}},
        {"Paste", MenuButtonEnabled{[](MainWindow& mw) { mw.controller().paste(); },
                                    [](MainWindow& mw) { return mw.controller().can_paste(); }}},
        {"", MenuSeparator{}},
        {"Select...", SubMenu{{
            // refactor: registered as `selection.filter_channels`; the stale MainWindow
            // forwarder was removed and the middle submenu level is gone.
            {"Filter channels", MenuButtonEnabled{
                [](MainWindow& mw) { mw.apply_function("selection.filter_channels"); },
                note_selected}},
        }}},
        {"", MenuSeparator{}},
        {"Transpose up", MenuButtonEnabled{[](MainWindow& mw) {
                                               mw.apply_function(
                                                   "note.transpose",
                                                   editor::EditFunctionArgs{}.set("amount", 1));
                                           },
                                           note_selected}},
        {"Transpose down", MenuButtonEnabled{[](MainWindow& mw) {
                                                 mw.apply_function(
                                                     "note.transpose",
                                                     editor::EditFunctionArgs{}.set("amount", -1));
                                             },
                                             note_selected}},
        {"", MenuSeparator{}},
        {"+1 Octave", MenuButtonEnabled{[](MainWindow& mw) {
                                            mw.apply_function(
                                                "note.transpose",
                                                editor::EditFunctionArgs{}.set("amount", 12));
                                        },
                                        note_selected}},
        {"-1 Octave", MenuButtonEnabled{[](MainWindow& mw) {
                                            mw.apply_function(
                                                "note.transpose",
                                                editor::EditFunctionArgs{}.set("amount", -12));
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
                 if (mw.plugins()) {
                     if (auto result = mw.plugins()->reload_plugins(); !result) {
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

    // refactor: Tools -> Editing is a projection of the edit-function registry now, so adding
    // a command needs no change in this file. plugins stay a separate mechanism; they keep
    // their own entry and are deliberately not routed through EditFunctionMenu.
    std::vector<std::pair<std::string, MenuItem>> tools_items;
    if (edit_function_menu_->has_group(EditFunctionMenu::default_group())) {
        // the single allowed submenu level: the command list is long.
        tools_items.emplace_back("Editing", MenuCustomContent{[this](MainWindow&) {
            edit_function_menu_->draw_group(EditFunctionMenu::default_group());
        }});
    }
    tools_items.emplace_back("Plugins", SubMenu{std::move(plugin_menu)});
    menu_bar_->add_menu("Tools", std::move(tools_items));

    menu_bar_->add_menu("Help", {
        {"Manual", MenuButton{[](MainWindow& mw) {
             mw.show_dialog(dialog_names::DIALOG_NAME_EDITOR_MANUAL);
         }}},
    });

    // refactor: the ad-hoc spacer and the broken search widget (it wrote into a zero-length
    // std::string buffer, so it could never accept input) are gone. MainMenuBar now owns one
    // inert right-aligned search stub for every caller - see draw_search_stub().
}
} // namespace andromeda::app
