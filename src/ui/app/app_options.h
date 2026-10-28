#pragma once

#include <filesystem>
#include <string>

namespace andromeda::app {

/// Command-line / launch-time configuration for the application.
///
/// REFACTOR: this used to be a nested `MainWindow::StartupOptions`, which forced
/// every translation unit that merely *parsed* the command line to include the
/// whole window facade. It is plain data, so it lives on its own now. The nested
/// name is kept as an alias so existing call sites keep compiling unchanged.
struct AppOptions {
    /// Project or MIDI file to open on startup; empty means "new project".
    std::filesystem::path file;

    /// Number of frames to render in the headless benchmark harness; 0 disables it.
    int bench_frames = 0;

    /// Route notes through the GPU cache instead of the CPU path.
    bool gpu_notes = true;

    /// Enable the note occlusion (cull) helper.
    bool note_cull = true;

    /// Enable the screen-coverage optimisation.
    bool coverage_cull = false;

    /// Hide the data view while benchmarking, to isolate the piano roll cost.
    bool bench_hide_data_view = false;

    /// Disable onion skinning while benchmarking.
    bool bench_no_onion = false;

    /// Start playback at this fraction of the song length before benchmarking.
    float bench_play_at = 0.0f;

    /// Override the horizontal zoom (in ticks) before benchmarking.
    float bench_zoom_ticks = 0.0f;

    /// Override the vertical zoom (in keys) before benchmarking.
    float bench_zoom_keys = 0.0f;

    /// Freeze the view (no zoom animation) while benchmarking.
    bool bench_still = false;

    /// When non-empty, write a PNG of the final frame to this path.
    std::string screenshot;
};

} // namespace andromeda::app
