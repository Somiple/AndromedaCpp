#include "managers.h"
#include "audio/midi_audio.h"
#include "audio/audio_timer.h"
#include "editor/timeline/timeline.h"
#include <iostream>

namespace andromeda::app::managers {

namespace {
using andromeda::audio::MIDIAudio;
using andromeda::audio::AudioTimer;
using andromeda::editor::timeline::Timeline;
}

void PlaybackManager::set_playback_start(editor::MIDITick start_tick) {
	if (!_timeline) { std::cout << "No timeline preset for setting playback start" << std::endl; return; }

	const double start_secs = _timeline->tick_to_secs(start_tick);
	set_playback_start(start_secs);

	// clear cached, so next time it's recieved, it's recalculated
	_cached_playback_start_tick = std::nullopt;
}

editor::MIDITick PlaybackManager::get_playback_start_ticks() const {
	if (!_timeline) { std::cout << "No timeline" << std::endl; return 0; }

	const std::uint64_t revision = _timeline->get_revision();
	if (_cached_playback_start_tick && _cached_timeline_revision == revision) {
		return *_cached_playback_start_tick;
	}

	const double start_secs = get_playback_start();
	editor::MIDITick start_ticks = _timeline->secs_to_tick(start_secs);
	_cached_playback_start_tick = start_ticks;
	_cached_timeline_revision = revision;
	return start_ticks;
}

editor::MIDITick PlaybackManager::get_playhead_tick_pos() const {
	if (_is_playing)
		return _timeline->secs_to_tick(_timer->elapsed());

	return get_playback_start_ticks();
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