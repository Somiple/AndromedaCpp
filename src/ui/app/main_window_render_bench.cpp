// REFACTOR: offline render bench extracted verbatim from main_window.cpp (lines 989-1310).
// This is a diagnostics-only code path (frame timing, screenshot dump) and shares no state
// with normal window operation beyond the members it already used. Behaviour unchanged.
#include "ui/app/main_window.h"

#include <glad/glad.h>
// glad must be included before glfw
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_internal.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>

#include <format>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string_view>
#include <tuple>

#include <stb_image_write.h>

#include "editor_app.h"
#include "managers.h"

#include "ui/rendering/data_view.h"
#include "ui/rendering/note_gpu_cache.h"
#include "ui/rendering/note_occlusion.h"
#include "ui/rendering/note_uploader.h"
#include "ui/rendering/piano_roll.h"
#include "ui/rendering/screen_coverage.h"
#include "ui/style/theme.h"
#include "ui/dialogs/crash_dialog.h"
#include "ui/dialogs/filter_channels.h"
#include "ui/dialogs/simple_dialog.h"
#include "ui/dialogs/editor_info.h"
#include "ui/dialogs/main_menu_bar.h"
#include "ui/dialogs/manual.h"
#include "ui/panels/panels.h"
#include "ui/layout/dock_panel.h"
#include "ui/widgets/data_view_widget.h"
#include "ui/widgets/piano_roll_widget.h"
#include "ui/widgets/track_view_widget.h"
#include "editor/plugins/plugin_andromeda_obj.h"
#include "editor/plugins/plugin_dialog.h"
#include "editor/plugins/plugin_error_dialog.h"
#include "editor/settings/editor_settings.h"
#include "editor/settings/project_settings.h"
#include "editor/editor_controller.h"
#include "editor/util.h"
#include "midi/midi_file.h"
#include "core/crash_handler.h"
#include "core/debugger.h"
namespace andromeda::app {

// REFACTOR: same alias main_window.cpp used before the split.
using util::Debugger;

void MainWindow::run_render_bench(int frames) {
    glfwSwapInterval(0);
    update_input_state();

    float zoom_ticks = 0.0f;
    {
        std::unique_lock lock(nav->mutex);
        nav->value.tick_pos = 0.0f;
        nav->value.tick_pos_smoothed = 0.0f;

        if (startup_.bench_zoom_ticks > 0.0f) {
            nav->value.zoom_ticks = startup_.bench_zoom_ticks;
            nav->value.zoom_ticks_smoothed = startup_.bench_zoom_ticks;
        }
        if (startup_.bench_zoom_keys > 0.0f) {
            nav->value.zoom_keys = startup_.bench_zoom_keys;
            nav->value.zoom_keys_smoothed = startup_.bench_zoom_keys;
            const float key_pos = std::max(0.0f, 64.0f - startup_.bench_zoom_keys * 0.5f);
            nav->value.key_pos = key_pos;
            nav->value.key_pos_smoothed = key_pos;
        }

        zoom_ticks = nav->value.zoom_ticks_smoothed;
    }
    {
        std::lock_guard lock(view_settings->mutex);
        view_settings->value.pr_autoscroll = startup_.bench_play_at > 0.0f;

        view_settings->value.pr_onion_state = startup_.bench_no_onion
                                                  ? VS_PianoRoll_OnionState::NoOnion
                                                  : VS_PianoRoll_OnionState::ViewAll;
    }

    if (startup_.bench_no_onion) {
        std::size_t busiest = 0;
        std::size_t most = 0;
        {
            auto tracks = editor_controller.get_project_manager()->get_tracks();
            for (std::size_t t = 0; t < tracks->size(); ++t) {
                const std::size_t n = tracks->at(t).get_notes().size();
                if (n > most) {
                    most = n;
                    busiest = t;
                }
            }
        }
        std::printf("bench draws track %zu alone (%zu notes)\n", busiest, most);
        // refactor: the selection now lives on EditorController; the bench uses the same
        // setter as the UI instead of writing the navigation mirror.
        editor_controller.set_active_track(static_cast<std::uint16_t>(busiest));
    }

    if (startup_.bench_hide_data_view) {
        std::lock_guard lock(view_settings->mutex);
        view_settings->value.pr_dataview_state = VS_PianoRoll_DataViewState::Hidden;
    }

    float song_ticks = 0.0f;
    {
        std::vector<midi::MIDITrack>& tracks = *editor_controller.get_project_manager()->get_tracks();
        for (midi::MIDITrack t : tracks) {
            if (!t.get_notes().empty()) {
                song_ticks = std::max(song_ticks, static_cast<float>(t.get_notes().back().start));
            }
        }
    }

    const float step = startup_.bench_still ? 0.0f : zoom_ticks / 60.0f;
    const float start_tick =
        std::max(0.0f, song_ticks * (startup_.bench_play_at > 0.0f ? startup_.bench_play_at : 0.4f));

    // i wasn't sure how this would've translated - pon
    if (startup_.bench_play_at > 0.0f) {
        const auto* timeline = editor_controller.get_project_manager()->get_timeline();
        auto* playback_manager = _app->get_playback_manager();
        if (!_app->get_playback_manager()->is_playing()) {
            playback_manager->set_playback_start(static_cast<midi::MIDITick>(std::llround(start_tick)));
            playback_manager->start_playback();
        } else {
            playback_manager->stop_playback();
        }
    }


    {
        std::shared_lock lock(nav->mutex);
        int fb_w = 0;
        int fb_h = 0;
        glfwGetFramebufferSize(window_, &fb_w, &fb_h);
        std::printf("view       %.0f ticks x %.1f keys from key %.1f, tick %.0f, %dx%d window\n",
                    nav->value.zoom_ticks_smoothed, nav->value.zoom_keys_smoothed,
                    nav->value.key_pos_smoothed, start_tick, fb_w, fb_h);
    }

    std::vector<double> gl_ms;
    std::vector<double> submit_ms;
    std::vector<double> frame_ms;
    std::uint64_t instances = 0;
    std::uint64_t coverage_tested = 0;
    std::uint64_t coverage_skipped = 0;
    std::uint64_t uploaded = 0;
    std::uint64_t resident_instances = 0;
    std::uint64_t cpu_instances = 0;
    int upload_frames = 0;
    std::size_t peak_instances = 0;
    double peak_instance_ms = 0.0;
    int peak_instance_frame = 0;

    struct FrameSample {
        double gl_ms = 0.0;
        double submit_ms = 0.0;
        std::size_t instances = 0;
        std::size_t uploaded = 0;
    };
    std::vector<FrameSample> samples;
    samples.reserve(static_cast<std::size_t>(frames));

    gl_ms.reserve(static_cast<std::size_t>(frames));
    frame_ms.reserve(static_cast<std::size_t>(frames));

    for (int i = 0; i < frames && glfwWindowShouldClose(window_) == GLFW_FALSE; ++i) {
        const auto frame_start = std::chrono::steady_clock::now();

        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        {
            std::unique_lock lock(nav->mutex);
            nav->value.tick_pos = start_tick + static_cast<float>(i) * step;
            nav->value.tick_pos_smoothed = nav->value.tick_pos;
        }

        mouse_over_ui_ = false;
        draw_ui();
        dialog_drawer_.draw_all_dialogs(image_resources);
        ImGui::Render();

        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.04f, 0.04f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        const auto gl_start = std::chrono::steady_clock::now();
        draw_gl_surface();
        const auto submit_end = std::chrono::steady_clock::now();
        glFinish();
        const auto gl_end = std::chrono::steady_clock::now();

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
        glfwSwapBuffers(window_);

        std::size_t uploaded_this_frame = 0;
        std::size_t drawn_this_frame = 0;
        if (auto* active = active_view().target_renderer.get()) {
            const std::size_t drawn = active->instances_drawn();
            drawn_this_frame = drawn;
            instances += drawn;
            if (drawn > peak_instances) {
                peak_instances = drawn;
                peak_instance_ms =
                    std::chrono::duration<double, std::milli>(gl_end - gl_start).count();
                peak_instance_frame = i;
            }
            resident_instances += active->resident_instances();
            cpu_instances += active->cpu_instances();
            coverage_tested += active->coverage_tested();
            coverage_skipped += active->coverage_skipped();
            uploaded_this_frame = active->take_uploaded_notes();
            uploaded += uploaded_this_frame;
            if (uploaded_this_frame > 0) {
                upload_frames += 1;
            }
        }

        constexpr int WARMUP_FRAMES = 10;
        if (i >= WARMUP_FRAMES) {
            gl_ms.push_back(std::chrono::duration<double, std::milli>(gl_end - gl_start).count());
            submit_ms.push_back(
                std::chrono::duration<double, std::milli>(submit_end - gl_start).count());
            samples.push_back(FrameSample{
                gl_ms.back(), submit_ms.back(),
                active_view().target_renderer.get() != nullptr ? drawn_this_frame : 0,
                uploaded_this_frame});
        }
        if (i >= 10) {
            frame_ms.push_back(std::chrono::duration<double, std::milli>(
                                   std::chrono::steady_clock::now() - frame_start)
                                   .count());
        }
    }

    // again, wasn't sure how to translate this
    if (startup_.bench_play_at > 0.0f) {
        auto* playback_manager = _app->get_playback_manager();
        if (!playback_manager->is_playing()) {
            playback_manager->start_playback();
        } else {
            playback_manager->stop_playback();
        }
    }

    if (gl_ms.empty()) {
        return;
    }

    if (!startup_.screenshot.empty()) {
        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        if (w > 0 && h > 0) {
            std::vector<std::uint8_t> pixels(static_cast<std::size_t>(w) *
                                             static_cast<std::size_t>(h) * 3);
            glPixelStorei(GL_PACK_ALIGNMENT, 1);
            glReadBuffer(GL_FRONT);
            glReadPixels(0, 0, w, h, GL_RGB, GL_UNSIGNED_BYTE, pixels.data());

            const std::size_t row = static_cast<std::size_t>(w) * 3;
            std::vector<std::uint8_t> flipped(pixels.size());
            for (int y = 0; y < h; ++y) {
                std::copy_n(pixels.begin() + static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(h - 1 - y)),
                            row, flipped.begin() + static_cast<std::ptrdiff_t>(row * static_cast<std::size_t>(y)));
            }

            if (stbi_write_png(startup_.screenshot.c_str(), w, h, 3, flipped.data(),
                               static_cast<int>(row)) == 0) {
                Debugger::log_error("Failed to write the bench screenshot");
            } else {
                std::printf("screenshot %s (%dx%d)\n", startup_.screenshot.c_str(), w, h);
            }
        }
    }

    const auto report = [](const char* what, std::vector<double> v) {
        std::sort(v.begin(), v.end());
        double sum = 0.0;
        for (const double x : v) {
            sum += x;
        }
        const double mean = sum / static_cast<double>(v.size());
        const double median = v[v.size() / 2];
        const double p95 = v[static_cast<std::size_t>(static_cast<double>(v.size()) * 0.95)];
        std::printf("%-10s mean %7.2f ms   median %7.2f ms   p95 %7.2f ms   max %7.2f ms   (%.0f fps at the mean)\n",
                    what, mean, median, p95, v.back(), 1000.0 / mean);
    };

    std::size_t total_notes = 0;
    {
        auto& tracks = *editor_controller.get_project_manager()->get_tracks();
        for (midi::MIDITrack& t : tracks) {
            total_notes += t.get_notes().size();
        }
    }

    std::printf("\n=== render bench: %d frames, %zu notes in project ===\n", frames, total_notes);
    std::printf("instances  %llu total, %.0f per frame\n",
                static_cast<unsigned long long>(instances),
                static_cast<double>(instances) / static_cast<double>(gl_ms.size()));
    std::printf("path       %llu instances from GPU-resident buffers, %llu packed by the CPU\n",
                static_cast<unsigned long long>(resident_instances),
                static_cast<unsigned long long>(cpu_instances));
    if (const auto* active = active_view().target_renderer.get()) {
        std::printf("residency  %zu MB resident; bails: %zu no buffer, %zu range past end, %zu empty range\n",
                    active->resident_megabytes(), active->residency_refusals(),
                    active->bail_range_past_end(), active->bail_empty_range());
        std::printf("tracks     %zu onion track draws resident, %zu fell back to the CPU\n",
                    active->resident_tracks(), active->cpu_tracks());
    }
    std::printf("coverage   %llu notes tested, %llu found hidden or off-screen (%.1f %%)\n",
                static_cast<unsigned long long>(coverage_tested),
                static_cast<unsigned long long>(coverage_skipped),
                coverage_tested > 0 ? 100.0 * static_cast<double>(coverage_skipped) /
                                          static_cast<double>(coverage_tested)
                                    : 0.0);
    std::printf("uploads    %llu notes over %d frames\n",
                static_cast<unsigned long long>(uploaded), upload_frames);
    std::printf("busiest    %zu instances in frame %d, which took %.2f ms\n", peak_instances,
                peak_instance_frame, peak_instance_ms);
    report("gl submit", submit_ms);
    report("gl total", gl_ms);
    report("frame", frame_ms);

    if (!samples.empty()) {
        std::vector<const FrameSample*> worst;
        worst.reserve(samples.size());
        for (const FrameSample& s : samples) {
            worst.push_back(&s);
        }
        std::partial_sort(worst.begin(),
                          worst.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(
                                              10, worst.size())),
                          worst.end(),
                          [](const FrameSample* a, const FrameSample* b) {
                              return a->gl_ms > b->gl_ms;
                          });

        std::printf("worst frames   %8s %8s %12s %12s\n", "gl ms", "submit", "instances",
                    "uploaded");
        for (std::size_t i = 0; i < std::min<std::size_t>(10, worst.size()); ++i) {
            std::printf("               %8.2f %8.2f %12zu %12zu\n", worst[i]->gl_ms,
                        worst[i]->submit_ms, worst[i]->instances, worst[i]->uploaded);
        }

        std::vector<const FrameSample*> busiest = worst;
        std::partial_sort(busiest.begin(),
                          busiest.begin() + static_cast<std::ptrdiff_t>(std::min<std::size_t>(
                                                5, busiest.size())),
                          busiest.end(),
                          [](const FrameSample* a, const FrameSample* b) {
                              return a->instances > b->instances;
                          });
        std::printf("busiest frames %8s %8s %12s %12s\n", "gl ms", "submit", "instances",
                    "uploaded");
        for (std::size_t i = 0; i < std::min<std::size_t>(5, busiest.size()); ++i) {
            std::printf("               %8.2f %8.2f %12zu %12zu\n", busiest[i]->gl_ms,
                        busiest[i]->submit_ms, busiest[i]->instances, busiest[i]->uploaded);
        }
    }
}
} // namespace andromeda::app
