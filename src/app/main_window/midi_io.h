#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

#include "editor/project/project_manager.h"
#include "midi/io.h"
#include "util/shared.h"

namespace andromeda::app {

class MIDIIoHandler {
public:
    MIDIIoHandler() = default;
    explicit MIDIIoHandler(util::SharedPtr<editor::ProjectManager> project_manager)
        : project_manager_(std::move(project_manager)) {}

    void rfd_import_midi();
    midi::MIDIParseStatus import_midi_file(const std::filesystem::path& path);

    void rfd_export_midi();
    void export_midi_file(const std::filesystem::path& path);

    std::optional<midi::MIDIParseStatus> get_last_parse_status();

    void on_files_dropped(std::vector<std::filesystem::path> paths);
    void handle_dropped_files();

private:
    util::SharedPtr<editor::ProjectManager> project_manager_;
    std::optional<midi::MIDIParseStatus> last_midi_load_status_;
    std::vector<std::filesystem::path> dropped_files_;
};

}
