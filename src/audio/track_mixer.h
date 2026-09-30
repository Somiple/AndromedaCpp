#pragma once

#include <cstdint>
#include <vector>

#include "midi/midi_track.h"
#include "util/shared.h"

namespace andromeda::audio {

class TrackMixer {
public:
    TrackMixer() : tracks_(util::make_shared_rw<std::vector<midi::MIDITrack>>()) {}

    explicit TrackMixer(util::SharedPtr<std::vector<midi::MIDITrack>> tracks)
        : tracks_(std::move(tracks)) {}

    void set_track_muted(std::uint16_t track, bool muted);

    void solo_track(std::uint16_t track);

    void unmute_all_tracks();

private:
    util::SharedPtr<std::vector<midi::MIDITrack>> tracks_;
};

}
