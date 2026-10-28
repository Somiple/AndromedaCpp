#pragma once

#include <expected>
#include <filesystem>
#include <string>

#include "editor/project/project_data.h"
#include "midi/io.h"

namespace andromeda::editor {

class ProjectManager {
public:
    ProjectManager() = default;

    void change_ppq(std::uint16_t new_ppq);

    [[nodiscard]] std::uint16_t get_ppq() const { return project_data.ppq; }

    midi::MIDIParseStatus import_from_midi_file(const std::string& path);

    std::expected<void, std::string> save_project(const std::filesystem::path& save_path);

    void new_empty_project();

    [[nodiscard]] Timeline* get_timeline() { return &project_data.timeline; }
    const Timeline* get_timeline() const { return &project_data.timeline; }

    // refactor: get_project_data_mut() and get_project_info_mut() were byte-identical
    // duplicates of the accessors here and were removed; their few call sites moved over.
    [[nodiscard]] ProjectData& get_project_data() { return project_data; }

    [[nodiscard]] ProjectInfo& get_project_info() { return project_info; }

    [[nodiscard]] std::vector<MetaEvent>* get_metas() { return &project_data.global_metas; }

    [[nodiscard]] std::vector<midi::MIDITrack>* get_tracks() {
        return &project_data.tracks;
    }

    // fixed rust bug: notes_only was ignored; true now skips channel events
    [[nodiscard]] bool is_project_empty(bool notes_only);

private:
    // refactor: the model was public so callers could reach it directly; it is private now and
    // the accessors above are the only way in. no call site used the members directly.
    ProjectData project_data;   ///< tracks, metas, ppq, timeline, mute_revision
    ProjectInfo project_info;   ///< name / author / description
};

}
