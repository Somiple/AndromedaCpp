#include "editor_controller.h"
#include "app/main_window.h"
#include "util/debugger.h"

namespace andromeda::editor {

constexpr uint16_t MAX_ACTIONS = 64;

using namespace andromeda::app;

EditorController::EditorController(MainWindow* main_window) :
	_project_manager(),
	_note_editing(main_window, this), _data_editing(main_window, this),
	_meta_editing(main_window, this), _track_editing(main_window, this),
	_edit_functions(), _actions(MAX_ACTIONS),
	_selected_notes(), _clipboard() { }

bool EditorController::can_undo() const { return _actions.get_can_undo(); }
bool EditorController::can_redo() const { return _actions.get_can_redo(); }
bool EditorController::can_copy() const { return _selected_notes.is_any_note_selected(); }
bool EditorController::can_paste() const { return !_clipboard.is_clipboard_empty(); }

void EditorController::undo() {
	if (!can_undo()) return;
	perform_action(_actions.undo_action());
}

void EditorController::redo() {
	if (!can_redo()) return;
	perform_action(_actions.redo_action());
}

void EditorController::copy() {
	if (!can_copy()) return;
	// TODO: copy for meta and channel events
	_note_editing.copy_notes(get_active_track());
}

void EditorController::cut() {
	if (!can_copy()) return;
	_note_editing.cut_selected_notes(get_active_track());
}

void EditorController::paste() {
	if (!can_paste()) return;
	_note_editing.paste_notes(get_active_track());
}


// call ONLY AFTER checking whether the function in question requires a dialog
void EditorController::perform_function(EditFunction function) {
	_edit_functions.apply_function(function, this);
}


void EditorController::perform_action(EditorAction* action) {
	if (action == nullptr) return;
	_note_editing.apply_action(*action);
	_meta_editing.apply_action(*action);
	_track_editing.apply_action(*action);
}

uint16_t EditorController::get_active_track() {
	return _active_track;
}

void EditorController::set_active_track(uint16_t track) {
	_project_manager.get_project_data_mut().validate_tracks(track);
	_active_track = track;
}

void EditorController::append_new_track() {
	auto* tracks = _project_manager.get_tracks();
	if (!tracks) return;

	// protection against more than 65,536 tracks
	{
		size_t track_count = tracks->size();
		if (track_count >= 0x10000)
		{
			util::Debugger::log_error("no track added; at max 65,536 track limit");
			return;
		}
	}

	_track_editing.append_empty_track();
}

void EditorController::remove_track(std::optional<uint16_t> track) {
	auto* tracks = _project_manager.get_tracks();
	if (!tracks || tracks->empty()) return;

	if (tracks->size() == 1) {
		util::Debugger::log_error("The project needs at least one track");
		return;
	}

	uint16_t track_to_remove = tracks->size() - 1;
	if (track) {
		if (*track >= tracks->size()) return;
		track_to_remove = *track;
	}

	_track_editing.remove_track(track_to_remove);
}

}