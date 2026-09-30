#include "audio/track_mixer.h"

namespace andromeda::audio {

void TrackMixer::set_track_muted(std::uint16_t track, bool muted) {
    std::unique_lock lock(tracks_->mutex);
    tracks_->value[static_cast<std::size_t>(track)].muted = muted;
}

void TrackMixer::solo_track(std::uint16_t track) {
    std::unique_lock lock(tracks_->mutex);
    for (std::size_t t = 0; t < tracks_->value.size(); ++t) {
        tracks_->value[t].muted = static_cast<std::uint16_t>(t) != track;
    }
}

void TrackMixer::unmute_all_tracks() {
    std::unique_lock lock(tracks_->mutex);
    for (midi::MIDITrack& track : tracks_->value) {
        track.muted = false;
    }
}

}
