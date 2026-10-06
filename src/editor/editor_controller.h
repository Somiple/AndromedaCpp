#pragma once

#include <memory>
#include "edit_functions.h"
#include "actions.h"
#include "editing.h"

#include "editing/note_editing.h"
#include "editing/data_editing.h"
#include "editing/meta_editing.h"
#include "editing/track_editing.h"
#include "project/project_manager.h"
#include "app/editor_tool.h"

namespace andromeda::app {
class MainWindow;
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

	void perform_function(EditFunction function);
	void perform_action(EditorAction* action);

	ProjectManager* get_project_manager() { return &_project_manager; }
	app::EditorToolSettings* get_editor_tool_settings() { return &_editor_tool; }
	app::ToolBarSettings* get_toolbar_settings() { return &_toolbar_settings; }

	editor::NoteEditing* get_note_editing() { return &_note_editing; }
	editor::DataEditing* get_data_editing() { return &_data_editing; }
	editor::MetaEditing* get_meta_editing() { return &_meta_editing; }
	editor::TrackEditing* get_track_editing() { return &_track_editing; }

	EditFunctions* get_edit_functions() { return &_edit_functions; }
	EditorActions* get_actions() { return &_actions; }
	SharedSelectedNotes* get_selection() { return &_selected_notes; }
	SharedClipboard* get_clipboard() { return &_clipboard; }

	uint16_t get_active_track();
	void set_active_track(uint16_t new_track);
private:
	ProjectManager _project_manager;
	app::EditorToolSettings _editor_tool{};
	app::ToolBarSettings _toolbar_settings{};

	editor::NoteEditing _note_editing;
	editor::DataEditing _data_editing;
	editor::MetaEditing _meta_editing;
	editor::TrackEditing _track_editing;

	EditFunctions _edit_functions;
	EditorActions _actions;
	SharedSelectedNotes _selected_notes;
	SharedClipboard _clipboard;

	uint16_t _active_track = 0;
};

}