#include "managers.h"

#include "audio/audio_timer.h"
#include "audio/midi_audio.h"
#include "core/debugger.h"
#include "editor/timeline/timeline.h"

namespace andromeda::app::managers {

namespace {
using andromeda::audio::AudioTimer;
using andromeda::audio::MIDIAudio;
using andromeda::editor::timeline::Timeline;
using andromeda::util::Debugger;
}

void PlaybackManager::set_playback_start(editor::MIDITick start_tick) {
	if (!_timeline) {
		// Console output used to sit here; diagnostics belong in the project log so
		// they reach the crash/log files instead of a non-existent terminal.
		Debugger::log_warning("No timeline preset for setting playback start");
		return;
	}

	const double start_secs = _timeline->tick_to_secs(start_tick);
	set_playback_start(start_secs);

	// clear cached, so next time it's recieved, it's recalculated
	_cached_playback_start_tick = std::nullopt;
}

editor::MIDITick PlaybackManager::get_playback_start_ticks() const {
	if (!_timeline) {
		Debugger::log_warning("No timeline");
		return 0;
	}

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
	// Sibling methods already guard their pointers; this one dereferenced blindly.
	if (_is_playing) {
		if (!_timeline || !_timer) {
			Debugger::log_warning("No timeline or timer for playhead position");
			return 0;
		}
		return _timeline->secs_to_tick(_timer->elapsed());
	}

	return get_playback_start_ticks();
}

void PlaybackManager::start_playback() {
	if (_is_playing) return;
	if (!_timer || !_engine || !_timeline) {
		Debugger::log_warning("Playback cannot start without a timeline, timer and audio engine");
		return;
	}

	_is_playing = true;

	_timer->start(_playback_start);
	_engine->start();
}

void PlaybackManager::stop_playback() {
	if (!_is_playing) return;

	_is_playing = false;

	// Only stop what actually started, so a partially initialised manager cannot
	// dereference a null timer/engine on shutdown.
	if (_timer) _timer->stop();
	if (_engine) _engine->stop();
}

void PlaybackManager::toggle_playback() {
	if (_is_playing) stop_playback();
	else start_playback();
}

void PlaybackManager::reset_playback() {
	if (_is_playing) stop_playback();
	if (_engine) _engine->reset();

	set_playback_start(0.0);
}

}
