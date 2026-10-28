// REFACTOR: boot sequence, GLFW/ImGui main loop and shutdown extracted verbatim from
// main_window.cpp (lines 1505-1768). This is the only TU that owns process lifetime;
// keeping it separate stops main_window.cpp from growing a second unrelated concern.
// Behaviour unchanged.
#include "ui/app/main_window.h"
// refactor: this TU drives the dropped-file queue, which now lives on ProjectActions. the
// forward declaration in main_window.h is not enough to call into it.
#include "ui/app/project_actions.h"

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

int MainWindow::run() {
    if (glfwInit() == GLFW_FALSE) {
        Debugger::log_error("Failed to initialise GLFW");
        return 1;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_DEPTH_BITS, 24);
    glfwWindowHint(GLFW_MAXIMIZED, GLFW_TRUE);

    window_ = glfwCreateWindow(1920, 1080, "Andromeda", nullptr, nullptr);
    if (window_ == nullptr) {
        Debugger::log_error("Failed to create the window");
        glfwTerminate();
        return 1;
    }

    glfwMakeContextCurrent(window_);
    glfwSwapInterval(1);

    glfwSetWindowUserPointer(window_, this);
    glfwSetDropCallback(window_, [](GLFWwindow* w, int count, const char** paths) {
        auto* self = static_cast<MainWindow*>(glfwGetWindowUserPointer(w));
        if (self == nullptr) {
            return;
        }

        std::vector<std::filesystem::path> dropped;
        dropped.reserve(static_cast<std::size_t>(count));
        for (int i = 0; i < count; ++i) {
            dropped.emplace_back(paths[i]);
        }
        // refactor: the drop queue moved from MIDIIoHandler to ProjectActions.
        self->project_actions().on_files_dropped(std::move(dropped));
    });

    if (gladLoadGLLoader(reinterpret_cast<GLADloadproc>(glfwGetProcAddress)) == 0) {
        Debugger::log_error("Failed to load OpenGL");
        glfwDestroyWindow(window_);
        glfwTerminate();
        return 1;
    }

    Debugger::log(std::format("OpenGL {}", reinterpret_cast<const char*>(glGetString(GL_VERSION))));
    {
        GLint depth_bits = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, GL_DEPTH,
                                              GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE, &depth_bits);
        Debugger::log(std::format("Depth buffer: {} bits", depth_bits));
    }

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;

    float xscale = 1.0f;
    float yscale = 1.0f;
    glfwGetWindowContentScale(window_, &xscale, &yscale);
    app_scale_ = xscale;
    apply_egui_dark_theme(app_scale_);

    {
        ImGuiIO& io = ImGui::GetIO();
        const float font_size = EGUI_BODY_FONT_SIZE * app_scale_;

        ImFontConfig cfg;
        cfg.OversampleH = 2;
        cfg.OversampleV = 1;
        cfg.PixelSnapH = true;

        if (io.Fonts->AddFontFromFileTTF("assets/fonts/Inter_18pt-Regular.ttf", font_size, &cfg,
                                         io.Fonts->GetGlyphRangesDefault()) != nullptr) {
            Debugger::log("UI font: assets/fonts/Inter_18pt-Regular.ttf");

            // do not merge a cjk font here: its metrics inflate the line height of all text
        } else {
            Debugger::log_warning(
                "assets/fonts/Ubuntu-Light.ttf missing; falling back to the system UI font.");
            io.Fonts->AddFontFromFileTTF("C:/Windows/Fonts/segoeui.ttf", font_size, &cfg,
                                         io.Fonts->GetGlyphRangesKorean());
        }

        io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;
    }

    ImGui_ImplGlfw_InitForOpenGL(window_, true);
    ImGui_ImplOpenGL3_Init("#version 330");

    load_image_resources();
    init_dialogs();
    init_dock_panels();
    build_menu_bar();
    init_widgets();

    timer_.start();

    rendering::NoteGpuCache::set_enabled(startup_.gpu_notes);
    rendering::NoteOcclusionStore::set_enabled(startup_.note_cull);
    rendering::ScreenCoverage::set_enabled(startup_.coverage_cull);

    if (!startup_.file.empty()) {
        project_actions().on_files_dropped({startup_.file});
    }

    if (const char* debug_dialog = std::getenv("ANDROMEDA_DEBUG_DIALOG");
        debug_dialog != nullptr && *debug_dialog != '\0') {
        show_dialog(debug_dialog);
    }

    if (const char* debug_view = std::getenv("ANDROMEDA_DEBUG_VIEW");
        debug_view != nullptr && std::string_view(debug_view) == "track") {
        set_render_type(RenderType::TrackView);
    }

    if (startup_.bench_frames > 0) {
        glfwPollEvents();
        project_actions().handle_dropped_files();
        if (const auto import_status = project_actions().take_last_parse_status()) {
            on_midi_loaded(*import_status);
        }

        run_render_bench(startup_.bench_frames);

        ImGui_ImplOpenGL3_Shutdown();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window_);
        glfwTerminate();
        return 0;
    }

    constexpr int SETTLE_FRAMES = 3;
    constexpr double IDLE_REDRAW_SECONDS = 0.25;
    int settle_frames = SETTLE_FRAMES;

    while (glfwWindowShouldClose(window_) == GLFW_FALSE) {
        flush_pending_refresh();
        if (settle_frames > 0 || wants_continuous_frames()) {
            glfwPollEvents();
        } else {
            const auto waited_from = std::chrono::steady_clock::now();
            glfwWaitEventsTimeout(IDLE_REDRAW_SECONDS);
            const double waited = std::chrono::duration<double>(
                                      std::chrono::steady_clock::now() - waited_from)
                                      .count();
            if (waited < IDLE_REDRAW_SECONDS * 0.9) {
                settle_frames = SETTLE_FRAMES;
            }
        }
        if (settle_frames > 0) {
            settle_frames -= 1;
        }

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();
        update_input_state();

        project_actions().handle_dropped_files();
        if (const auto import_status = project_actions().take_last_parse_status()) {
            on_midi_loaded(*import_status);
        }

        static const bool frame_log = std::getenv("ANDROMEDA_FRAME_LOG") != nullptr;
        const auto frame_begin = std::chrono::steady_clock::now();

        mouse_over_ui_ = false;

        if (!has_crashed_) {
            try {
                handle_key_inputs();
                draw_ui();
                dialog_drawer_.draw_all_dialogs(image_resources);
                process_closed_dialogs();
            } catch (const std::exception& e) {
                util::set_last_panic(e.what());
                Debugger::log_error(std::format("Frame aborted: {}", e.what()));
                has_crashed_ = true;
            } catch (...) {
                util::set_last_panic("Unknown panic");
                has_crashed_ = true;
            }
        } else {
            if (!crash_dlg_shown_) {
                Debugger::log_error("Oh no! Andromeda has crashed :(");
                show_dialog(dialog_names::DIALOG_NAME_CRASH);
                crash_dlg_shown_ = true;
            }

            dialog_drawer_.draw_all_dialogs(image_resources);
        }

        ImGui::Render();

        int w = 0;
        int h = 0;
        glfwGetFramebufferSize(window_, &w, &h);
        glViewport(0, 0, w, h);
        glClearColor(0.04f, 0.04f, 0.04f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);

        const auto ui_done = std::chrono::steady_clock::now();

        draw_gl_surface();

        const auto gl_done = std::chrono::steady_clock::now();

        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window_);

        if (frame_log) {
            const auto swapped = std::chrono::steady_clock::now();
            const auto ms = [](auto a, auto b) {
                return std::chrono::duration<double, std::milli>(b - a).count();
            };

            static double worst = 0.0;
            static double worst_ui = 0.0;
            static double worst_gl = 0.0;
            static double worst_swap = 0.0;
            static std::size_t worst_instances = 0;
            static std::size_t worst_cpu = 0;
            static int frames_this_second = 0;
            static auto second_start = frame_begin;

            const double total = ms(frame_begin, swapped);
            if (total > worst) {
                worst = total;
                worst_ui = ms(frame_begin, ui_done);
                worst_gl = ms(ui_done, gl_done);
                worst_swap = ms(gl_done, swapped);
                if (const auto* active = active_view().target_renderer.get()) {
                    worst_instances = active->instances_drawn();
                    worst_cpu = active->cpu_instances();
                }
            }
            frames_this_second += 1;

            if (ms(second_start, swapped) >= 1000.0) {
                Debugger::log(std::format(
                    "[frame] {} fps, worst {:.1f} ms (ui {:.1f}, notes {:.1f}, swap {:.1f}), "
                    "{} instances, {} of them packed by the CPU",
                    frames_this_second, worst, worst_ui, worst_gl, worst_swap, worst_instances,
                    worst_cpu));
                worst = 0.0;
                worst_instances = 0;
                worst_cpu = 0;
                frames_this_second = 0;
                second_start = swapped;
            }
        }
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window_);
    glfwTerminate();

    return 0;
}
} // namespace andromeda::app
