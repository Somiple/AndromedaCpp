#include "app/main_window/midi_io.h"

#include <algorithm>
#include <chrono>
#include <execution>
#include <format>
#include <mutex>
#include <shared_mutex>
#include <windows.h>
#include <shobjidl.h>

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

bool same_drive(const std::filesystem::path& a, const std::filesystem::path& b) {
    const std::wstring ra = a.root_name().wstring();
    const std::wstring rb = b.root_name().wstring();
    return !ra.empty() && _wcsicmp(ra.c_str(), rb.c_str()) == 0;
}

// Moves src to dst, across volumes if needed. It copies to "<dst>.part" first and then
// renames that into place, so the real target never holds a half-written file.
bool move_into_place(const std::filesystem::path& src, const std::filesystem::path& dst,
    std::string& error) {
    std::filesystem::path part = dst;
    part += L".part";

    if (!MoveFileExW(src.c_str(), part.c_str(), MOVEFILE_COPY_ALLOWED | MOVEFILE_REPLACE_EXISTING)) {
        error = std::format("could not copy to {} (Windows error {})", part.string(), GetLastError());
        return false;
    }
    if (!MoveFileExW(part.c_str(), dst.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        error = std::format("could not rename into {} (Windows error {})", dst.string(), GetLastError());
        return false;
    }
    return true;
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

    Debugger::log(std::format("Starting import of {}", path.filename().string()));
    const float start = elapsed_secs(import_timer);
    {
        import_result = project_manager_->import_from_midi_file(path.string());
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
    using clk = std::chrono::steady_clock;

    Debugger::log(std::format("Starting export of {}", path.filename().string()));
    const auto export_timer = clk::now();
    const float start = elapsed_secs(export_timer);

    // std::shared_lock pm_lock(project_manager_->mutex);
    // TODO: lock the project manager
    const std::uint16_t ppq = project_manager_->get_ppq();

    const std::vector<midi::MetaEvent>& global_metas = *project_manager_->get_metas();
    std::vector<midi::MIDITrack>& tracks = *project_manager_->get_tracks();

    std::vector<std::size_t> indices(tracks.size());
    for (std::size_t i = 0; i < indices.size(); ++i) {
        indices[i] = i;
    }
    std::vector<midi::MIDIFileWriter::EncodedTrack> encoded(tracks.size());

    std::for_each(std::execution::par, indices.begin(), indices.end(), [&](std::size_t i) {
        const auto& notes = tracks[i].get_notes();
        const unsigned inner = notes.size() > 1'000'000 ? 0 : 1;
        const std::string_view name = tracks[i].name ? std::string_view(*tracks[i].name) : std::string_view{};
        encoded[i] = midi::MIDIFileWriter::encode_track(
            notes, tracks[i].get_channel_evs(), inner, name);
    });

    midi::MIDIFileWriter midi_writer(ppq);
    midi_writer.flush_global_metas(global_metas);
    for (auto& e : encoded) {
        midi_writer.append_encoded_track(std::move(e));
    }

    const std::filesystem::path target = std::filesystem::absolute(path);
    const std::filesystem::path temp_dir = std::filesystem::temp_directory_path();

    const bool direct = same_drive(target, temp_dir);
    const std::filesystem::path out =
        direct ? target
        : temp_dir / std::format("andromeda_export_{}.tmp",
            std::chrono::steady_clock::now().time_since_epoch().count());

    if (const auto written = midi_writer.write_midi(out); !written.has_value()) {
        Debugger::log_error(std::format("Failed to write {}: {}", path.string(), written.error()));
        std::error_code ec;
        std::filesystem::remove(out, ec);
        return;
    }

    const float end = elapsed_secs(export_timer);
    Debugger::log(std::format("Exported MIDI to path {} in {}s", out.string(), end - start));

    if (!direct) {
        move_thread_ = std::jthread([out, target] {
            Debugger::log(std::format("Moving exported MIDI to {}", target.string()));
            std::string error;

            const auto move_clock = clk::now();
            const float move_start = elapsed_secs(move_clock);
            if (move_into_place(out, target, error)) {
                const float move_end = elapsed_secs(move_clock);
                Debugger::log(std::format("Move completed in {}", move_end - move_start));
            } else {
                Debugger::log_error(std::format("{}. The export is still at {}", error, out.string()));
            }
        });
    }
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
