#pragma once
#include "context_popup.h"

namespace andromeda::editor {
class TrackEditing;
}

namespace andromeda::app {
class MainWindow;
}

namespace andromeda::ui::popups {

// the popup menu that appears when right clicking on a track
class TrackContextPopup : public ContextPopup {
public:
	TrackContextPopup(app::MainWindow* main_window,
		editor::TrackEditing* track_editing);
	void show(std::uint16_t target_track);
private:
	app::MainWindow* main_window_;
	editor::TrackEditing* track_editing_;
	std::uint16_t target_track = 0;
};

}