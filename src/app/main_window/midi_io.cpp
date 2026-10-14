#include "app/main_window/midi_io.h"

#include <algorithm>
#include <chrono>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <system_error>

#include <windows.h>
#include <shobjidl.h>

#include "midi/midi_export.h"
#include "midi/midi_file.h"
#include "util/debugger.h"

namespace andromeda::app {

using util::Debugger;

namespace {

std::filesystem::path shell_file_dialog(bool save, const wchar_t* filter_name,
                                        const wchar_t* filter_spec) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    const bool needs_uninit = SUCCEEDED(init);

    std::filesystem::path result;

    IFileDialog* dialog = nullptr;
    const HRESULT created =
        CoCreateInstance(save ? CLSID_FileSaveDialog : CLSID_FileOpenDialog, nullptr,
                         CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog));

    if (SUCCEEDED(created) && dialog != nullptr) {
        const COMDLG_FILTERSPEC spec[] = {{filter_name, filter_spec}};
        dialog->SetFileTypes(1, spec);
        dialog->SetFileTypeIndex(1);

        if (SUCCEEDED(dialog->Show(nullptr))) {
            IShellItem* item = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) && item != nullptr) {
                PWSTR path = nullptr;
                if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path != nullptr) {
                    result = std::filesystem::path(path);
                    CoTaskMemFree(path);
                }
                item->Release();
            }
        }

        dialog->Release();
    } else {
        Debugger::log_error("Failed to create the shell file dialog");
    }

    if (needs_uninit) {
        CoUninitialize();
    }

    return result;
}

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

}

void MIDIIoHandler::rfd_import_midi() {
    const std::filesystem::path file = shell_file_dialog(false, L"MIDI Files", L"*.mid;*.midi");

    const midi::MIDIParseStatus parse_status =
        !file.empty() ? import_midi_file(file) : midi::MIDIParseStatus::ParseNotMIDI;

    last_midi_load_status_ = parse_status;
}

midi::MIDIParseStatus MIDIIoHandler::import_midi_file(const std::filesystem::path& path) {
    midi::MIDIParseStatus import_result = midi::MIDIParseStatus::ParseOK;

    const auto import_timer = std::chrono::steady_clock::now();

    Debugger::log(std::format("Starting import of {}", utf8_of(path.filename())));
    const float start = elapsed_secs(import_timer);
    {
        import_result = project_manager_->import_from_midi_file(utf8_of(path));
    }
    const float end = elapsed_secs(import_timer);
    Debugger::log(std::format("Imported MIDI in {}s", end - start));

    return import_result;
}

void MIDIIoHandler::rfd_export_midi() {
    const std::filesystem::path file = shell_file_dialog(true, L"MIDI Files", L"*.mid");

    if (!file.empty()) {
        export_midi_file(file);
    }
}

void MIDIIoHandler::export_midi_file(const std::filesystem::path& path) {
    Debugger::log(std::format("Starting export of {}", utf8_of(path.filename())));
    const auto export_timer = std::chrono::steady_clock::now();

    // TODO: lock the project manager. the exporter reads the notes more than once, so an
    // edit during the export is not only wrong output
    const std::uint16_t ppq = project_manager_->get_ppq();
    const std::vector<midi::MetaEvent>& global_metas = *project_manager_->get_metas();
    const std::vector<midi::MIDITrack>& tracks = *project_manager_->get_tracks();

    if (const auto written = midi::export_midi_file(path, ppq, global_metas, tracks); !written) {
        Debugger::log_error(std::format("Failed to export: {}", written.error()));
        return;
    }

    Debugger::log(std::format("Exported MIDI in {}s", elapsed_secs(export_timer)));
}

std::optional<midi::MIDIParseStatus> MIDIIoHandler::get_last_parse_status() {
    const auto status = last_midi_load_status_;
    last_midi_load_status_.reset();
    return status;
}

void MIDIIoHandler::on_files_dropped(std::vector<std::filesystem::path> paths) {
    dropped_files_ = std::move(paths);
}

void MIDIIoHandler::handle_dropped_files() {
    if (dropped_files_.empty()) {
        return;
    }

    if (dropped_files_.size() > 1) {
        Debugger::log_warning("User dropped more than one file, using the first one available!");
    }

    last_midi_load_status_ = import_midi_file(dropped_files_[0]);
    dropped_files_.clear();
}

}
