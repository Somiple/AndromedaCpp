// refactor: new translation unit. the shell file picker was a file-local helper inside
// ui/app/midi_io.cpp; it is now reusable by any UI code that needs a path (project save
// will use it next) and keeps COM/windows headers out of the calling code.

#include "ui/util/native_file_dialog.h"

#include <windows.h>
#include <shobjidl.h>

#include "core/debugger.h"

namespace andromeda::app {

namespace {

/**
 * @brief Runs the shell open/save dialog and returns the chosen path.
 * @param save true for CLSID_FileSaveDialog, false for CLSID_FileOpenDialog.
 * @param filter_name human readable filter label.
 * @param filter_spec shell pattern list.
 * @return chosen path, empty when cancelled or on failure.
 */
std::filesystem::path shell_file_dialog(bool save, const wchar_t* filter_name,
                                        const wchar_t* filter_spec) {
    // moved verbatim from ui/app/midi_io.cpp: COM is initialised per call and torn down
    // again because the dialog itself is short lived.
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
        util::Debugger::log_error("Failed to create the shell file dialog");
    }

    if (needs_uninit) {
        CoUninitialize();
    }

    return result;
}

} // namespace

std::filesystem::path native_open_file_dialog(const wchar_t* filter_name,
                                              const wchar_t* filter_spec) {
    return shell_file_dialog(false, filter_name, filter_spec);
}

std::filesystem::path native_save_file_dialog(const wchar_t* filter_name,
                                              const wchar_t* filter_spec) {
    return shell_file_dialog(true, filter_name, filter_spec);
}

} // namespace andromeda::app
