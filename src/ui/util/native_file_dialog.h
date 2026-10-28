#pragma once

// ---------------------------------------------------------------------------
// Layer: ui
// Native (Windows shell) file picker, isolated from the project/MIDI logic that
// asks for a path. Callers pass the filter text; the helper owns COINIT and the
// COM plumbing.
// ---------------------------------------------------------------------------

#include <filesystem>

namespace andromeda::app {

/**
 * @brief Shows the shell "open file" dialog.
 * @param filter_name human readable filter label, e.g. L"MIDI Files".
 * @param filter_spec shell pattern, e.g. L"*.mid;*.midi".
 * @return the chosen path, or an empty path when the user cancelled or the dialog failed.
 */
[[nodiscard]] std::filesystem::path native_open_file_dialog(const wchar_t* filter_name,
                                                            const wchar_t* filter_spec);

/**
 * @brief Shows the shell "save file" dialog.
 * @param filter_name human readable filter label, e.g. L"MIDI Files".
 * @param filter_spec shell pattern, e.g. L"*.mid".
 * @return the chosen path, or an empty path when the user cancelled or the dialog failed.
 */
[[nodiscard]] std::filesystem::path native_save_file_dialog(const wchar_t* filter_name,
                                                            const wchar_t* filter_spec);

} // namespace andromeda::app
