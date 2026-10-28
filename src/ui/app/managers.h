#pragma once

#include "editor/midi_types.h"
#include <optional>

namespace andromeda::audio {
class MIDIAudio;
class AudioTimer;
}

namespace andromeda::editor::timeline {
class Timeline;
}

namespace andromeda::app::managers {

class PlaybackManager {
public:
	explicit PlaybackManager(
		audio::MIDIAudio* engine, audio::AudioTimer* timer,
		editor::timeline::Timeline* timeline)
		: _engine(engine), _timer(timer), _timeline(timeline) { }

	void set_playback_start(double start_secs) { _playback_start = start_secs; }
	void set_playback_start(editor::MIDITick start_tick);

	[[nodiscard]] double get_playback_start() const { return _playback_start; }
	// get "playhead"'s start time in ticks
	[[nodiscard]] editor::MIDITick get_playback_start_ticks() const;

	/// Current playback position in ticks: while playing this follows the timer,
	/// otherwise it reports the (cached) playback start tick.
	/// Resolves the former TODO asking for a tick-domain playback query.
	[[nodiscard]] editor::MIDITick get_playhead_tick_pos() const;

	[[nodiscard]] const bool is_playing() const { return _is_playing; }

	void start_playback();
	void stop_playback();
	void toggle_playback();

	// resets playback, and sets the time to zero
	void reset_playback();

private:
	audio::MIDIAudio* _engine;
	audio::AudioTimer* _timer;
	editor::timeline::Timeline* _timeline;

	bool _is_playing = false;

	double _playback_start = 0.0;
	mutable std::optional<editor::MIDITick> _cached_playback_start_tick{};
	mutable std::uint64_t _cached_timeline_revision = 0;
};


}