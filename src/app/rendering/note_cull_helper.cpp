#include "app/rendering/note_cull_helper.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <shared_mutex>

namespace andromeda::app::rendering {

namespace {
constexpr float NAN_F = std::numeric_limits<float>::quiet_NaN();
}

NoteCullHelper::NoteCullHelper(std::vector<midi::MIDITrack>* tracks) {
    std::size_t n_tracks = tracks->size();

    first_render_.assign(n_tracks, 0);
    end_render_.assign(n_tracks, 0);
    last_start_.assign(n_tracks, 0);
    last_time_.assign(n_tracks, NAN_F);
    last_zoom_.assign(n_tracks, NAN_F);
}

void NoteCullHelper::update_cull_for_track(std::vector<midi::MIDITrack>& tracks,
                                           std::uint16_t track_id, float time, float zoom,
                                           bool force) {
    sync_cull_array_lengths(tracks);

    const auto track = static_cast<std::size_t>(track_id);
    if (track >= tracks.size()) {
        return;
    }

    const std::vector<midi::Note>& notes = tracks[track].get_notes();
    if (notes.empty()) {
        return;
    }

    if (force) {
        first_render_[track] = 0;
        end_render_[track] = 0;
    }

    std::size_t n_off = first_render_[track];

    // fixed rust bug: a window left past the end of a shortened track panicked in rust
    if (n_off > notes.size() || end_render_[track] > notes.size()) {
        n_off = 0;
        last_time_[track] = NAN_F;
    }

    const bool first_call = std::isnan(last_time_[track]);

    if (!first_call && last_time_[track] > time) {
        if (n_off == 0) {
            for (const midi::Note& note : notes) {
                if (static_cast<float>(note.end()) > time) {
                    break;
                }
                n_off += 1;
            }
        } else {
            for (std::size_t i = n_off; i-- > 0;) {
                if (static_cast<float>(notes[i].end()) <= time) {
                    break;
                }
                n_off -= 1;
            }
        }

        first_render_[track] = n_off;
    } else if (first_call || last_time_[track] < time) {
        for (std::size_t i = n_off; i < notes.size(); ++i) {
            if (static_cast<float>(notes[i].end()) > time) {
                break;
            }
            n_off += 1;
        }
        first_render_[track] = n_off;
    }

    const auto first = notes.begin() + static_cast<std::ptrdiff_t>(n_off);
    const auto it = std::partition_point(first, notes.end(), [&](const midi::Note& note) {
        return static_cast<float>(note.get_start()) <= time + zoom;
    });

    end_render_[track] = n_off + static_cast<std::size_t>(it - first);

    last_time_[track] = time;
    last_zoom_[track] = zoom;
}

std::pair<std::size_t, std::size_t> NoteCullHelper::get_track_cull_range(std::uint16_t track) {
    const auto idx = static_cast<std::size_t>(track);
    if (idx >= first_render_.size()) {
        return {0, 0};
    }
    return {first_render_[idx], end_render_[idx]};
}

void NoteCullHelper::sync_cull_array_lengths(const std::vector<midi::MIDITrack>& tracks) {
    const std::size_t n_tracks = tracks.size();

    if (last_start_.size() != n_tracks) {
        last_start_.assign(n_tracks, 0);
        first_render_.assign(n_tracks, 0);
        end_render_.assign(n_tracks, 0);
        last_time_.assign(n_tracks, NAN_F);
        last_zoom_.assign(n_tracks, NAN_F);
    }
}

}
