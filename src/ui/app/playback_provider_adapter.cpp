#include "playback_provider_adapter.h"

#include "managers.h"

namespace andromeda::app {

PlaybackProviderAdapter::PlaybackProviderAdapter(managers::PlaybackManager& playback) noexcept
    : _playback(playback) { }

editor::PlaybackSnapshot PlaybackProviderAdapter::snapshot() const {
    editor::PlaybackSnapshot out;
    // get_playhead_tick_pos() already follows the timer while playing and falls back to the
    // cached start tick otherwise, so no extra branching is needed here.
    out.playhead_tick = static_cast<double>(_playback.get_playhead_tick_pos());
    out.playing = _playback.is_playing();
    return out;
}

double PlaybackProviderAdapter::playback_start_tick() const {
    return static_cast<double>(_playback.get_playback_start_ticks());
}

void PlaybackProviderAdapter::seek(double tick) {
    _playback.set_playback_start(static_cast<editor::MIDITick>(tick));
}

void PlaybackProviderAdapter::set_playing(bool playing) {
    if (playing) {
        _playback.start_playback();
    } else {
        _playback.stop_playback();
    }
}

} // namespace andromeda::app
