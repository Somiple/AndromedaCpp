#pragma once

// ---------------------------------------------------------------------------
// Layer: editor
// Dependency direction:  core -> midi -> editor -> audio -> ui
// This layer may include `core/...` and `midi/...` only; it must not include
// ImGui, GLFW or GL headers. The `ui/app/` includes below are the known
// exception (tool/toolbar settings and the dialog manager) and are scheduled to
// move into `editor/` once the app facade stops owning them.
// See docs/ARCHITECTURE.md.
// ---------------------------------------------------------------------------

#include <memory>
#include <string_view>
#include "edit_function_registry.h"
#include "midi/events/meta_event.h"
#include "editing.h"

#include "editing/note_editing.h"
#include "editing/data_editing.h"
#include "editing/meta_editing.h"
#include "editing/track_editing.h"
#include "project/project_manager.h"
#include "ui/app/editor_tool.h"
#include "editor/history/transaction.h"

namespace andromeda::app {
class MainWindow;
class DialogManager;
}

namespace andromeda::editor {

class EditorController {
public:
	EditorController(app::MainWindow* main_window);

	[[nodiscard]] bool can_undo() const;
	[[nodiscard]] bool can_redo() const;

	[[nodiscard]] bool can_copy() const;
	[[nodiscard]] bool can_paste() const;

	void undo();
	void redo();

	void copy();
	void cut();
	void paste();

	/// Runs one registered editor command by id.
	/// @param id stable command id, e.g. "note.transpose".
	/// @return true when it ran; false when unknown, disabled or missing required parameters.
	[[nodiscard]] bool perform_function(std::string_view id);

	/// Runs a registered editor command with values collected by the parameters dialog.
	/// @param id stable command id.
	/// @param args values gathered from the command's declared EditParam list.
	/// @return true when it ran; false when unknown or disabled.
	[[nodiscard]] bool perform_function(std::string_view id, const EditFunctionArgs& args);

	/// Injects the dialog manager used to open editor-owned dialogs.
	/// The controller does not own it; pass `nullptr` to detach.
	/// @param dialog_manager Window-owned dialog manager, may be null in headless use.
	void set_dialog_manager(app::DialogManager* dialog_manager) { _dialog_manager = dialog_manager; }

	/// Opens the insert-meta dialog for tempo / time-signature events.
	/// Moved here from `MainWindow`: creating meta events is an editing concern.
	/// @param meta_type Event kind to create; other kinds are ignored.
	/// @param playhead_tick Tick the new event is inserted at.
	void insert_meta(midi::MetaEventType meta_type, MIDITick playhead_tick);

	/// Opens the channel-filter dialog for the current selection.
	/// Moved here from `MainWindow`: filtering a selection is an editing concern.
	void filter_selection_channels();

	void append_new_track();
	void remove_track(std::optional<uint16_t> track = std::nullopt);

	/// Currently selected track index. Single source of truth for track selection;
	/// every UI surface reads this instead of keeping its own copy.
	[[nodiscard]] std::uint16_t get_active_track() const;

	/// How many tracks the project currently uses, as reported by the track pass.
	/// Resolves the former panel-level TODO that reached into `TrackEditing`.
	[[nodiscard]] std::uint16_t get_used_track_count() const;
	void set_active_track(uint16_t new_track);

	ProjectManager* get_project_manager() { return &_project_manager; }
	const ProjectManager* get_project_manager() const { return &_project_manager; }

	app::EditorToolSettings* get_editor_tool_settings() { return &_editor_tool; }
	app::ToolBarSettings* get_toolbar_settings() { return &_toolbar_settings; }

	editor::NoteEditing* get_note_editing() { return &_note_editing; }
	editor::DataEditing* get_data_editing() { return &_data_editing; }
	editor::MetaEditing* get_meta_editing() { return &_meta_editing; }
	editor::TrackEditing* get_track_editing() { return &_track_editing; }
	history::ActionHistory* get_history() { return &_history; }
	history::ActionExecutor* get_executor() { return &_executor; }
	[[nodiscard]] history::Transaction begin_action(std::string action_name) {
		return _recorder.begin(std::move(action_name));
	}

	[[nodiscard]] EditFunctionRegistry& edit_functions() { return _edit_function_registry; }
	[[nodiscard]] const EditFunctionRegistry& edit_functions() const { return _edit_function_registry; }
	SharedSelectedNotes* get_selection() { return &_selected_notes; }
	SharedClipboard* get_clipboard() { return &_clipboard; }
private:
	ProjectManager _project_manager;
	app::EditorToolSettings _editor_tool{};
	app::ToolBarSettings _toolbar_settings{};

	editor::NoteEditing _note_editing;
	editor::DataEditing _data_editing;
	editor::MetaEditing _meta_editing;
	editor::TrackEditing _track_editing;

	EditFunctionRegistry _edit_function_registry;
	SharedSelectedNotes _selected_notes;
	SharedClipboard _clipboard;

	history::ActionExecutor _executor;
	history::ActionHistory  _history;
	history::ActionRecorder _recorder;

	uint16_t _active_track = 0;

	// injected, not owned
	app::DialogManager* _dialog_manager = nullptr;
};

}