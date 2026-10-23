#pragma once

namespace andromeda::audio {
class MIDIAudio;
class AudioTimer;
}

namespace andromeda::app::managers {

class PlaybackManager {
public:
	explicit PlaybackManager(
		audio::MIDIAudio* engine, audio::AudioTimer* timer)
		: _engine(engine), _timer(timer) { }

	void set_playback_start(double start_secs) { _playback_start = start_secs; }
	[[nodiscard]] double get_playback_start() const { return _playback_start; }

	[[nodiscard]] const bool is_playing() const { return _is_playing; }

	void start_playback();
	void stop_playback();
	void toggle_playback();

	// resets playback, and sets the time to zero
	void reset_playback();
private:
	audio::MIDIAudio* _engine;
	audio::AudioTimer* _timer;

	bool _is_playing = false;
	double _playback_start = 0.0;
};


}