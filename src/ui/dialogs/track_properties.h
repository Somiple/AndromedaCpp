#pragma once

#include "ui/dialogs/dialog.h"
#include "midi/midi_track.h"
#include <array>

namespace andromeda::app {

class TrackPropertiesDialog : public Dialog {
public:
	void on_open(const DialogPayload& payload) override;

	[[nodiscard]] const char* get_title() const override;
	void draw_content() override;
private:
	midi::MIDITrack* track;
	ImColor color_code_;

	mutable std::string title_;
};

}