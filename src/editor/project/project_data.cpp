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

    reset_solo_state();

    timeline.rebuild(global_metas, ppq);
}

void ProjectData::reset_solo_state() {
    // the saved mute flags belong to the old tracks; keeping them made the next un-solo restore
    // another file's mutes (or index past the end)
    _persisted_muted_tracks.clear();
    _soloed_track = 0;
    _is_soloed = false;
    bump_mute_revision();
}

void ProjectData::reset_or_init_data() {
    reset_solo_state();
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

    timeline.rebuild(global_metas, ppq);
}

void ProjectData::validate_tracks(std::uint16_t track) {
    Debugger::log_warning("validate_tracks() deprecated. validate them with transaction api instead");
}

void ProjectData::set_track_muted(uint16_t track, bool muted) {
    if (track >= tracks.size()) return;
    tracks[track].muted = muted;
    bump_mute_revision();
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
        bump_mute_revision();
        return;
    }

    if (!_is_soloed) {
        return;
    }

    for (std::size_t i = 0; i < tracks.size(); ++i) {
        // tracks may have been added since the solo
        tracks[i].muted = i < _persisted_muted_tracks.size() && _persisted_muted_tracks[i];
    }

    reset_solo_state();
}

}