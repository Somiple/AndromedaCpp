#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "midi/midi_track.h"
#include "util/shared.h"

// callers already hold the tracks read lock; relocking here can deadlock

namespace andromeda::app::rendering {

class NoteCullHelper {
public:
    NoteCullHelper() = default;
    explicit NoteCullHelper(const util::SharedPtr<std::vector<midi::MIDITrack>>& tracks);

    void update_cull_for_track(const std::vector<midi::MIDITrack>& tracks, std::uint16_t track,
                               float time, float zoom, bool force);

    [[nodiscard]] std::pair<std::size_t, std::size_t> get_track_cull_range(std::uint16_t track);

    void sync_cull_array_lengths(const std::vector<midi::MIDITrack>& tracks);

private:
    std::vector<std::size_t> first_render_;
    std::vector<std::size_t> end_render_;
    std::vector<std::size_t> last_start_;

    std::vector<float> last_time_;
    std::vector<float> last_zoom_;
};

}
