#include <cmath>
#include <limits>

#include "editor_controller.h"
#include "ui/app/main_window.h"
#include "ui/dialogs/dialog.h"
#include "ui/dialogs/dialog_manager.h"
#include "core/debugger.h"
// refactor: the catalog is the single extension point for editor commands.
#include "editor/editing/edit_function_catalog.h"

namespace andromeda::editor {

constexpr uint16_t MAX_ACTIONS = 64;

using namespace andromeda::app;

EditorController::EditorController(MainWindow* main_window) :
	_project_manager(),
	// REFACTOR: components take the narrow context, not the window, so src/editor/ no longer
	// names app::MainWindow.
	_note_editing(&main_window->editor_context(), this),
	_data_editing(&main_window->editor_context(), this),
	_meta_editing(&main_window->editor_context(), this),
	_track_editing(&main_window->editor_context(), this),
	_edit_function_registry(), _executor({&_project_manager, &_selected_notes, &_active_track}),
	_history(history::ActionHistory::Limits{MAX_ACTIONS}),
	_recorder(&_history, &_executor),
	_selected_notes(), _clipboard() {
	// refactor: one registration pass replaces the former EditFunction enum + switch.
	register_builtin_edit_functions(_edit_function_registry);
}

MIDITick EditorController::playhead_tick() const {
	// refactor: commands used to receive the playhead as a parameter because nothing in the
	// editor layer could reach the transport. the controller now carries the provider the ui
	// layer owns, so the read is a single call and the parameter is gone.
	if (_playback == nullptr) {
		return 0;
	}
	const double tick = _playback->snapshot().playhead_tick;
	// the transport reports a fractional position while rolling; commands edit whole ticks, so
	// round to the nearest tick rather than truncating, which would bias every command left.
	if (!(tick > 0.0)) {
		return 0;   // also rejects NaN
	}
	constexpr double max_tick = static_cast<double>(std::numeric_limits<MIDITick>::max());
	if (tick >= max_tick) {
		return std::numeric_limits<MIDITick>::max();
	}
	return static_cast<MIDITick>(std::llround(tick));
}

bool EditorController::can_undo() const { return _history.can_undo(); }
bool EditorController::can_redo() const { return _history.can_redo(); }
bool EditorController::can_copy() const { return _selected_notes.is_any_note_selected(); }
bool EditorController::can_paste() const { return !_clipboard.is_clipboard_empty(); }

void EditorController::undo() { _history.undo(_executor); }
void EditorController::redo() { _history.redo(_executor); }

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


// refactor: the registry now enforces the dialog requirement, so the caller no longer has to
// check whether a command needs one - the no-argument overload refuses parameterised entries.
bool EditorController::perform_function(std::string_view id) {
	return _edit_function_registry.invoke(id, *this);
}

bool EditorController::perform_function(std::string_view id, const EditFunctionArgs& args) {
	return _edit_function_registry.invoke(id, args, *this);
}

std::uint16_t EditorController::get_active_track() const {
	return _active_track;
}

std::uint16_t EditorController::get_used_track_count() const {
	// Thin forwarder: the panel used to reach into TrackEditing itself.
	return _track_editing.get_used_track_count();
}

void EditorController::set_active_track(uint16_t track) {
	std::vector<midi::MIDITrack>* tracks = _project_manager.get_tracks();
	if (!tracks) return;

	const std::size_t before = tracks->size();
	const std::size_t wanted = static_cast<std::size_t>(track) + 1;

	// only the empty tail beyond the new active track is trimmed
	std::size_t keep = before;
	while (keep > wanted && tracks->at(keep - 1).get_notes().empty() &&
	       tracks->at(keep - 1).get_channel_evs().empty()) {
		--keep;
	}

	// plain switch: no undo step, no transaction
	if (wanted == before || (wanted < before && keep == before)) {
		_active_track = track;
		return;
	}

	// tracks are added/removed: record the active track change in the same action, so undo and
	// redo restore it together with the tracks
	auto tx = begin_action("Change track");
	history::TrackMutator tm = tx.tracks();

	if (wanted > before) {
		for (std::size_t i = before; i < wanted; ++i) tm.add_track();
	} else {
		for (std::size_t i = before; i-- > keep;)  // descending so indices stay valid
			tm.remove_track(static_cast<uint16_t>(i));
	}
	tm.finalize_tracks();
	tx.set_active_track(track);

	if (!tx.commit()) {
		util::Debugger::log_error("Failed to commit track change");
	}
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

void EditorController::insert_meta(midi::MetaEventType meta_type, MIDITick playhead_tick) {
	// REFACTOR: moved out of MainWindow - creating meta events is an editing concern,
	// the window only forwarded its playhead position and dialog manager.
	if (meta_type != midi::MetaEventType::TimeSignature &&
		meta_type != midi::MetaEventType::Tempo) {
		return;
	}

	if (!_dialog_manager) {
		util::Debugger::log_warning("Cannot insert meta event without a dialog manager");
		return;
	}

	auto meta_dialog = std::make_unique<MetaEventInsertDialog>();

	MetaEditing* meta_editing = &_meta_editing;

	meta_dialog->init_meta_dialog(meta_type, [meta_editing, playhead_tick, meta_type](
												 std::vector<std::uint8_t> data) {
		meta_editing->insert_meta_event(
			midi::MetaEvent{ playhead_tick, meta_type, std::move(data) });
	});

	_dialog_manager->open_dialog(std::move(meta_dialog), {});
}

void EditorController::filter_selection_channels() {
	// REFACTOR: moved out of MainWindow - filtering a selection is an editing concern.
	if (!_dialog_manager) {
		util::Debugger::log_warning("Cannot filter selection without a dialog manager");
		return;
	}

	DialogArgs args;
	args.emplace_back(this);

	_dialog_manager->open_dialog_by_name(app::dialog_names::DIALOG_NAME_FILTER_CHANNELS,
										 std::move(args));
}

}