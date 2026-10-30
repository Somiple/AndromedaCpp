// refactor: new translation unit. the project-session operations that used to be
// MainWindow members live here now. MainWindow no longer knows how a project is created
// or saved; it only wires this coordinator up in its constructor. behaviour is unchanged:
// same prompt text, same dialog ids, same reset sequence, same status strings.

#include "ui/app/project_actions.h"

#include <chrono>
#include <cstdint>
#include <format>
#include <system_error>
#include <utility>

#include "core/debugger.h"
#include "midi/io/midi_file_io.h"
#include "ui/util/native_file_dialog.h"

namespace andromeda::app {

namespace {

// refactor: moved with the midi flows from ui/app/midi_io.cpp — unchanged helpers.
float elapsed_secs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - since).count();
}

// path::string throws for names the ansi code page cannot hold; utf-8 holds every valid one
std::string utf8_of(const std::filesystem::path& path) {
    try {
        const std::u8string name = path.u8string();
        return std::string(reinterpret_cast<const char*>(name.data()), name.size());
    } catch (const std::system_error&) {
        // a name that is not valid unicode has no utf-8 form
        return "?";
    }
}

} // namespace

ProjectActions::ProjectActions(ProjectActionsContext ctx) : m_ctx(std::move(ctx)) {}

void ProjectActions::request_new_project() {
    // was MainWindow::make_new_project. an empty project has nothing to lose, so the
    // confirmation prompt is skipped in that case (same short-circuit as before).
    if (m_ctx.project.is_project_empty(false)) {
        return;
    }

    DialogArgs args;
    args.emplace_back(std::string("Confirmation"));
    args.emplace_back(std::string("Are you sure you want to start a new project?"));
    args.emplace_back(std::string(kNewProjectConfirmationId));

    m_ctx.open_dialog(dialog_names::DIALOG_NAME_SIMPLE, std::move(args));
}

void ProjectActions::create_new_project() {
    // was the "NewProjectConfirmation" branch of MainWindow::process_closed_dialogs.
    util::Debugger::log("Clearning notes...");
    m_ctx.project.new_empty_project();

    // history, playback and viewport position are window-owned, so the window performs
    // them through the callback instead of this coordinator reaching into its state.
    util::Debugger::log("Removing action history...");
    util::Debugger::log("Stopping playback (if any)...");
    m_ctx.on_project_reset();

    m_ctx.set_status("New project");
}

void ProjectActions::save_project() {
    // was MainWindow::save_project, which is still a stub waiting on a file dialog.
    const std::string message = "Save Project is not wired up yet (needs the file dialog)";
    m_ctx.set_status(message);
    util::Debugger::log_warning(message);
}

void ProjectActions::import_midi() {
    // was MainWindow::import_midi -> MIDIIoHandler::rfd_import_midi. the prompts belong to
    // the ui, the parsing belongs to the project manager; this only sequences the two.
    const std::filesystem::path file = native_open_file_dialog(L"MIDI Files", L"*.mid;*.midi");

    const midi::MIDIParseStatus parse_status =
        !file.empty() ? import_midi_file(file) : midi::MIDIParseStatus::ParseNotMIDI;

    last_midi_load_status_ = parse_status;
}

void ProjectActions::export_midi() {
    // was MainWindow::export_midi -> MIDIIoHandler::rfd_export_midi.
    std::filesystem::path file = native_save_file_dialog(L"MIDI Files", L"*.mid");

    if (!file.empty()) {
        // automatically add .mid extension
        if (file.extension() != ".mid") file += ".mid";
        export_midi_file(file);
    }
}

midi::MIDIParseStatus ProjectActions::import_midi_file(const std::filesystem::path& path) {
    const auto import_timer = std::chrono::steady_clock::now();

    util::Debugger::log(std::format("Starting import of {}", utf8_of(path.filename())));
    const float start = elapsed_secs(import_timer);
    const midi::MIDIParseStatus import_result = m_ctx.project.import_from_midi_file(utf8_of(path));
    const float end = elapsed_secs(import_timer);
    util::Debugger::log(std::format("Imported MIDI in {}s", end - start));

    return import_result;
}

void ProjectActions::export_midi_file(const std::filesystem::path& path) {
    util::Debugger::log(std::format("Starting export of {}", utf8_of(path.filename())));
    const auto export_timer = std::chrono::steady_clock::now();

    // TODO: lock the project manager. the exporter reads the notes more than once, so an
    // edit during the export is not only wrong output
    const std::uint16_t ppq = m_ctx.project.get_ppq();
    const std::vector<midi::MetaEvent>& global_metas = *m_ctx.project.get_metas();
    const std::vector<midi::MIDITrack>& tracks = *m_ctx.project.get_tracks();

    // refactor: the byte-level write now goes through midi/io/midi_file_io instead of
    // calling midi::export_midi_file directly from the ui layer.
    const midi::MidiIoResult written =
        midi::MidiFileIo::export_to_path(path, ppq, global_metas, tracks);
    if (!written.ok) {
        util::Debugger::log_error(std::format("Failed to export: {}", written.message));
        return;
    }

    util::Debugger::log(std::format("Exported MIDI in {}s", elapsed_secs(export_timer)));
}

void ProjectActions::on_files_dropped(std::vector<std::filesystem::path> paths) {
    dropped_files_ = std::move(paths);
}

void ProjectActions::handle_dropped_files() {
    if (dropped_files_.empty()) {
        return;
    }

    if (dropped_files_.size() > 1) {
        util::Debugger::log_warning(
            "User dropped more than one file, using the first one available!");
    }

    last_midi_load_status_ = import_midi_file(dropped_files_[0]);
    dropped_files_.clear();
}

std::optional<midi::MIDIParseStatus> ProjectActions::take_last_parse_status() {
    const auto status = last_midi_load_status_;
    last_midi_load_status_.reset();
    return status;
}

std::uint16_t ProjectActions::ppq() const {
    // was MainWindow::get_ppq. project timing is project state, so the query lives with the
    // project coordinator; the window keeps a forwarder for the existing call sites.
    return m_ctx.project.get_ppq();
}

bool ProjectActions::handle_confirmed_dialog(std::string_view dialog_id) {
    if (dialog_id != kNewProjectConfirmationId) {
        return false;
    }

    create_new_project();
    return true;
}

} // namespace andromeda::app
