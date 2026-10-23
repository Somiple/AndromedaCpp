#include "managers.h"
#include "audio/midi_audio.h"
#include "audio/audio_timer.h"

namespace andromeda::app::managers {

namespace {
using andromeda::audio::MIDIAudio;
using andromeda::audio::AudioTimer;
}

void PlaybackManager::start_playback() {
	if (_is_playing) return;
	_is_playing = true;

	_timer->start(_playback_start);
	_engine->start();
}

void PlaybackManager::stop_playback() {
	if (!_is_playing) return;
	_is_playing = false;

	_timer->stop();
	_engine->stop();
}

void PlaybackManager::toggle_playback() {
	if (_is_playing) stop_playback();
	else start_playback();
}

void PlaybackManager::reset_playback() {
	if (_is_playing) stop_playback();
	_engine->reset();

	set_playback_start(0.0);
}

}