#include "track_context.h"
#include "editor/editing/track_editing.h"
#include "editor/editor_controller.h"
#include "core/debugger.h"
#include "ui/app/main_window.h"

namespace {
using andromeda::util::Debugger;
}

namespace andromeda::ui::popups {

TrackContextPopup::TrackContextPopup(app::MainWindow* main_window,
	editor::TrackEditing* track_editing)
	: ContextPopup("track_context", {
		{ "Properties...", [this]() { 
			midi::MIDITrack* track_ptr =
				&track_editing_->controller()->get_project_manager()->get_tracks()->at(target_track);

			main_window_->open_dialog("track_properties",
				app::DialogPayload{ track_ptr });
		} },
		{},
		{ "Insert above", [this]() {
			track_editing_->insert_track(target_track);
		} },
		{ "Insert below", [this]() {
			if (target_track == 0xFFFF) {
				Debugger::log_warning("Can't insert a track after 65536.");
				return;
			}

			track_editing_->insert_track(target_track + 1);
		} },
		{},
		{ "Move up", [this]() {
			if (target_track == 0) return;
			track_editing_->swap_tracks(target_track, target_track - 1);
		} },
		{ "Move down", [this]() {
			if (target_track + 1 >= track_editing_->get_used_track_count())
				track_editing_->insert_track(target_track);
			else
				track_editing_->swap_tracks(target_track, target_track + 1);
		} },
		{},
		{ "Decompose", [this]() {
			track_editing_->decompose_track(target_track);
		} },
		{},
		{ "Delete", [this]() {
			track_editing_->remove_track(target_track);
		} }
	}), main_window_(main_window), track_editing_(track_editing) { }

void TrackContextPopup::show(std::uint16_t target_track) {
	Debugger::log(std::format("[popup] show({}) used={}", target_track,
		track_editing_ ? track_editing_->get_used_track_count() : -1));
	if (track_editing_ == nullptr) return;
	if (target_track >= track_editing_->get_used_track_count()) return;
	this->target_track = target_track;
	ContextPopup::show();
}

}