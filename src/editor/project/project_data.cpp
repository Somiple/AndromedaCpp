#include "editor/project/project_data.h"

#include <format>
#include <utility>

#include "editor/util.h"
#include "util/debugger.h"

namespace andromeda::editor {

using util::Debugger;

void ProjectData::load_data_from_midi_file(midi::MIDIFile& midi_file) {
    // TODO: mutex here
    ppq = midi_file.ppq;

    midi_file.preprocess_meta_events();

    global_metas = std::move(midi_file.global_meta_events);
    midi_file.global_meta_events.clear();
    tracks = std::move(midi_file.tracks);
    midi_file.tracks.clear();

    tempo_map.rebuild_tempo_map(ppq);
}

void ProjectData::reset_or_init_data() {
    // TODO: mutex here
    tracks.clear();
    tracks.push_back(midi::MIDITrack::new_empty());

    {
        const auto tempo_bytes = tempo_as_bytes(120.0f);

        global_metas = {
            MetaEvent{.tick = 0,
                      .event_type = MetaEventType::Tempo,
                      .data = {tempo_bytes.begin(), tempo_bytes.end()}},
            MetaEvent{.tick = 0,
                      .event_type = MetaEventType::KeySignature,
                      .data = {0x00, 0x00}},
            MetaEvent{.tick = 0,
                      .event_type = MetaEventType::TimeSignature,
                      .data = {0x04, 0x02, 0x18, 0x08}}
        };
    }

    tempo_map.meta_events_ptr = &global_metas;
    tempo_map.rebuild_tempo_map(960);

    validate_tracks(0);
}

void ProjectData::validate_tracks(std::uint16_t track) {
    // TODO: mutex here

    const std::size_t last_len = tracks.size();
    const std::int32_t new_len = static_cast<std::int32_t>(track) + 1;
    const std::int32_t len_change = new_len - static_cast<std::int32_t>(last_len);
    if (len_change == 0) {
        return;
    }

    if (len_change < 0) {
        for (std::int32_t i = 0; i < -len_change; ++i) {
            const bool can_remove = !tracks.empty() && tracks.back().is_empty();

            if (can_remove) {
                tracks.pop_back();
            } else {
                break;
            }
        }
    } else {
        for (std::int32_t i = 0; i < len_change; ++i) {
            tracks.push_back(midi::MIDITrack::new_empty());
        }
    }

    Debugger::log(std::format("Using {} tracks", tracks.size()));
}

void ProjectData::set_track_muted(uint16_t track, bool muted) {
    if (track >= tracks.size()) return;
    tracks[track].muted = muted;
}

void ProjectData::solo_track(uint16_t track, bool solo) {
    if (track >= tracks.size()) {
        return;
    }

    if (solo && _is_soloed && _soloed_track == track) solo = false;

    if (solo) {
        if (!_is_soloed) {
            _persisted_muted_tracks.clear();
            _persisted_muted_tracks.reserve(tracks.size());

            for (const auto& trk : tracks) {
                _persisted_muted_tracks.push_back(trk.muted);
            }

            _is_soloed = true;
        }

        for (std::size_t i = 0; i < tracks.size(); ++i) {
            tracks[i].muted = i != track;
        }

        _soloed_track = track;
        return;
    }

    if (!_is_soloed) {
        return;
    }

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        tracks[i].muted = _persisted_muted_tracks[i];
    }

    _persisted_muted_tracks.clear();
    _soloed_track = 0;
    _is_soloed = false;
}

}
