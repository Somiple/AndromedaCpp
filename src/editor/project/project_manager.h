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

    std::expected<void, std::string> save_project(const std::filesystem::path& save_path) const;

    void new_empty_project();

    [[nodiscard]] const util::SharedPtr<TempoMap>& get_tempo_map() const {
        return project_data.tempo_map;
    }

    [[nodiscard]] const ProjectData& get_project_data() const { return project_data; }
    ProjectData& get_project_data_mut() { return project_data; }

    [[nodiscard]] const ProjectInfo& get_project_info() const { return project_info; }
    ProjectInfo& get_project_info_mut() { return project_info; }

    [[nodiscard]] const SharedMetaEvents& get_metas() const { return project_data.global_metas; }

    [[nodiscard]] const util::SharedPtr<std::vector<midi::MIDITrack>>& get_tracks() const {
        return project_data.tracks;
    }

    // fixed rust bug: notes_only was ignored; true now skips channel events
    [[nodiscard]] bool is_project_empty(bool notes_only) const;

    ProjectData project_data;
    ProjectInfo project_info;
    bool ppq_changed = false;
};

}
