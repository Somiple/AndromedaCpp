#include "editor/project/project_manager.h"

#include "editor/project.h"
#include "midi/midi_file.h"
#include "util/debugger.h"

namespace andromeda::editor {

using util::Debugger;

void ProjectManager::change_ppq(std::uint16_t new_ppq) {
    // TODO: lock
    project_data.ppq = new_ppq;
    project_data.tempo_map.rebuild_tempo_map(new_ppq);

    ppq_changed = true;
}

midi::MIDIParseStatus ProjectManager::import_from_midi_file(const std::string& path) {
    midi::MIDIFile midi_file;

    midi_file.with_track_discarding(false);
    if (auto r = midi_file.open(path); r) {
        project_data.load_data_from_midi_file(midi_file);
        return midi::MIDIParseStatus::ParseOK;
    } else {
        Debugger::log_error(r.error());
        return midi::MIDIParseStatus::ParseError;
    }
}

std::expected<void, std::string> ProjectManager::save_project(
    const std::filesystem::path& save_path) {
    ProjectWriter project_writer(this, save_path);

    if (auto r = project_writer.write_header(); !r) {
        return r;
    }
    if (auto r = project_writer.finalize(); !r) {
        return r;
    }

    Debugger::log("Project saved!");
    return {};
}

void ProjectManager::new_empty_project() {
    project_data.reset_or_init_data();

    project_info.name = "";
    project_info.author = "";
    project_info.description = "";
    project_data.ppq = 960;
}

bool ProjectManager::is_project_empty(bool notes_only) {
    for (midi::MIDITrack& track : project_data.tracks) {
        if (notes_only ? !track.get_notes().empty() : !track.is_empty()) {
            return false;
        }
    }
    return true;
}

}
