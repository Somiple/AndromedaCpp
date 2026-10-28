#pragma once

// refactor: new file. project-session operations used to live on MainWindow
// (make_new_project / save_project); they now belong to this coordinator.

#include <cstdint>
#include <functional>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/project/project_manager.h"
#include "midi/io.h"
#include "ui/dialogs/dialog.h"

namespace andromeda::app {

/**
 * @brief Narrow dependency bundle for ProjectActions.
 *
 * References and callbacks only: deliberately contains no MainWindow back-pointer, so the
 * coordinator cannot reach window state it has no business touching.
 */
struct ProjectActionsContext {
    /// Project data and persistence; owned by the editor layer and only borrowed here.
    editor::ProjectManager& project;

    /// Opens a dialog by name, closing any dialogs already on the stack. Must match
    /// MainWindow::show_dialog_with_args so the prompts look exactly as they did before.
    std::function<void(const char* name, DialogArgs args)> open_dialog;

    /// Window-side reset performed after a project swap (undo history, playback, viewport).
    /// Kept as a callback so this coordinator owns no widgets and no render state.
    std::function<void()> on_project_reset;

    /// Publishes a human readable status line into the window chrome.
    std::function<void(const std::string&)> set_status;
};

/**
 * @brief Owns the project-session flows that used to be MainWindow members.
 *
 * Before this class, MainWindow::make_new_project and MainWindow::save_project mixed
 * window state (status text, dialog stack, viewport reset) with project-domain policy.
 * All project data work is delegated to ProjectManager; this coordinator only sequences
 * the prompts, the post-swap reset and the status text.
 *
 * @note Never throws: failures are logged and reported through the context callbacks.
 */
class ProjectActions {
public:
    /// Dialog id used to recognise the "start a new project" confirmation reply.
    static constexpr const char* kNewProjectConfirmationId = "NewProjectConfirmation";

    /**
     * @brief Stores the dependency bundle.
     * @param ctx narrow dependency bundle; every reference and callback must outlive this object.
     */
    explicit ProjectActions(ProjectActionsContext ctx);

    /**
     * @brief Asks for confirmation when the project holds data, then opens the prompt.
     *        Was MainWindow::make_new_project.
     */
    void request_new_project();

    /**
     * @brief Performs the confirmed new-project swap: data reset, window reset, status text.
     *        Was the "NewProjectConfirmation" branch of MainWindow::process_closed_dialogs.
     */
    void create_new_project();

    /**
     * @brief Records that project saving is not wired to a file dialog yet.
     *        Was MainWindow::save_project.
     */
    void save_project();

    /**
     * @brief Prompts for a .mid file and imports it into the current project.
     *        Was MainWindow::import_midi (which only forwarded to the MIDI handler).
     */
    void import_midi();

    /**
     * @brief Prompts for a .mid destination and exports the current project.
     *        Was MainWindow::export_midi (which only forwarded to the MIDI handler).
     */
    void export_midi();

    /**
     * @brief Queues a dropped file so the next frame imports it.
     *        Was MIDIIoHandler::on_files_dropped; the drop comes from a GLFW callback.
     * @param paths dropped paths; only the first one is used, as before.
     */
    void on_files_dropped(std::vector<std::filesystem::path> paths);

    /**
     * @brief Imports the queued drop, if any. Was MIDIIoHandler::handle_dropped_files.
     *        Called once per frame from the window loop.
     */
    void handle_dropped_files();

    /**
     * @brief Returns and clears the pending parse status.
     *        Was MIDIIoHandler::get_last_parse_status; the window polls it once per frame
     *        to run its own post-import refresh.
     * @return the status of the last import, or std::nullopt when nothing was imported.
     */
    [[nodiscard]] std::optional<midi::MIDIParseStatus> take_last_parse_status();

    /**
     * @brief Pulses-per-quarter of the current project.
     *        Was MainWindow::get_ppq, which did nothing but forward to the project manager.
     * @return the project's PPQ; 0 when no project has been loaded.
     */
    [[nodiscard]] std::uint16_t ppq() const;

    /**
     * @brief Routes an OK'd simple dialog to the project action that owns its id.
     * @param dialog_id id of the dialog that just closed with OK.
     * @return true when the id belongs to this coordinator and was handled.
     */
    [[nodiscard]] bool handle_confirmed_dialog(std::string_view dialog_id);

private:
    /**
     * @brief Imports @p path through the project manager.
     * @param path source .mid file.
     * @return the project manager's parse status.
     */
    midi::MIDIParseStatus import_midi_file(const std::filesystem::path& path);

    /**
     * @brief Writes the current project to @p path through midi::MidiFileIo.
     * @param path destination .mid file.
     */
    void export_midi_file(const std::filesystem::path& path);

    ProjectActionsContext m_ctx;   ///< borrowed references and callbacks, see ProjectActionsContext

    // refactor: moved here with the MIDI flows. the drop queue and the polled status used
    // to be MIDIIoHandler members; keeping them next to the import they feed avoids a
    // second tiny handler class that existed only to hold two fields.
    std::optional<midi::MIDIParseStatus> last_midi_load_status_;   ///< pending status, consumed by take_last_parse_status()
    std::vector<std::filesystem::path> dropped_files_;             ///< files queued by the GLFW drop callback
};

} // namespace andromeda::app
