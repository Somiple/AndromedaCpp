cmake_minimum_required(VERSION 3.25)

# vcpkg toolchain must be set before project(). Honour an externally supplied
# CMAKE_TOOLCHAIN_FILE, otherwise fall back to the copy bundled with Visual Studio.
if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
    if(DEFINED ENV{VCPKG_ROOT})
        set(CMAKE_TOOLCHAIN_FILE "$ENV{VCPKG_ROOT}/scripts/buildsystems/vcpkg.cmake"
            CACHE STRING "Vcpkg toolchain file")
    endif()
endif()

project(Andromeda
    VERSION 2.6.0
    DESCRIPTION "Andromeda MIDI editor"
    LANGUAGES CXX)


set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")

option(ANDROMEDA_NATIVE_ARCH "Build for AVX2-capable CPUs (anything from ~2013)" ON)
option(ANDROMEDA_BUILD_TOOLS "Also build the port verification tools in tools/" OFF)

if(MSVC)
    add_compile_options(/utf-8 /permissive- /Zc:preprocessor /W4 /wd4100 /wd4201 /EHsc)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN _CRT_SECURE_NO_WARNINGS)

    string(REPLACE "/Ob1" "" CMAKE_CXX_FLAGS_RELWITHDEBINFO "${CMAKE_CXX_FLAGS_RELWITHDEBINFO}")
    add_compile_options(
        "$<$<CONFIG:Release,RelWithDebInfo>:/Ob3;/Oi;/Ot;/GL;/Gw>")
    add_link_options(
        "$<$<CONFIG:Release,RelWithDebInfo>:/LTCG;/OPT:REF;/OPT:ICF>")

    if(ANDROMEDA_NATIVE_ARCH)
        add_compile_options("$<$<CONFIG:Release,RelWithDebInfo>:/arch:AVX2>")
    endif()
endif()

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(nlohmann_json CONFIG REQUIRED)
find_package(CURL REQUIRED)
find_package(RtMidi CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(imgui CONFIG REQUIRED)
find_package(OpenGL REQUIRED)
find_package(Lua REQUIRED)
find_package(sol2 CONFIG REQUIRED)

# ---------------------------------------------------------------------------
# Libraryss
# ---------------------------------------------------------------------------
add_library(andromeda_core STATIC
    src/util/util.cpp
    src/util/debugger.cpp
    src/util/crash_handler.cpp
    src/util/system_stats.cpp
    src/util/expression_parser.cpp

    src/midi/events/meta_event.cpp
    src/midi/events/mergers.cpp
    src/midi/io/buffered_reader.cpp
    src/midi/io/mapped_file.cpp
    src/midi/midi_track_parser.cpp
    src/midi/midi_file.cpp

    src/editor/util.cpp
    src/editor/navigation.cpp
    src/editor/tempo_map.cpp
    src/editor/editing.cpp
    src/editor/project.cpp
    src/editor/project/project_data.cpp
    src/editor/project/project_manager.cpp
    src/editor/midi_bar_cacher.cpp
    src/editor/actions.cpp
    src/editor/editing/meta_editing/meta_sequence_funcs.cpp
    src/editor/editing/note_editing/note_sequence_funcs.cpp
    src/editor/editing/note_editing.cpp
    src/editor/editing/data_editing.cpp
    src/editor/editing/meta_editing.cpp
    src/editor/editing/track_editing.cpp
    src/editor/editing/lua_note_editing.cpp
    src/editor/plugins.cpp
    src/editor/plugins/plugin_lua.cpp
    src/editor/plugins/plugin_andromeda_obj.cpp
    src/editor/plugins/plugin_dialog.cpp
    src/editor/plugins/plugin_error_dialog.cpp
    src/editor/playhead.cpp
    src/editor/edit_functions.cpp
    src/editor/settings/editor_settings.cpp
    src/editor/settings/project_settings.cpp

    src/audio/track_mixer.cpp
    src/audio/kdmapi_engine.cpp
    src/audio/midi_devices.cpp
    src/audio/event_playback.cpp
    src/audio/prerender_engine/limiter.cpp
    src/audio/soft_synth.cpp
    src/audio/prerender_engine.cpp

    src/app/theme.cpp
    src/app/panel_layout.cpp
    src/app/shared.cpp
    src/app/util/image_loader.cpp
    src/app/rendering.cpp
    src/app/rendering/shaders.cpp
    src/app/rendering/buffers.cpp
    src/app/rendering/note_cull_helper.cpp
    src/app/rendering/note_gpu_cache.cpp
    src/app/rendering/note_uploader.cpp
    src/app/rendering/note_occlusion.cpp
    src/app/rendering/screen_coverage.cpp
    src/app/rendering/piano_roll.cpp
    src/app/rendering/track_view.cpp
    src/app/rendering/data_view.cpp
    src/app/ui/dialog_manager.cpp
    src/app/ui/dialogs/simple_dialog.cpp
    src/app/ui/dialogs/crash_dialog.cpp
    src/app/ui/dialogs/filter_channels.cpp
    src/app/ui/editor_info.cpp
    src/app/ui/manual.cpp
    src/app/ui/dialog_drawer.cpp
    src/app/ui/main_menu_bar.cpp
    src/app/ui/panels.cpp
    src/app/main_window/midi_io.cpp
    src/app/main_window.cpp
    "src/app/ui/dock_panels/dock_panel.h"
    "src/app/ui/dock_panels/dock_manager.cpp" "src/editor/editor_controller.cpp" "src/editor/editor_component.cpp" "src/util/math/vector2.cpp" "src/util/collections/chunk_vec.cpp")

target_include_directories(andromeda_core PUBLIC src ${LUA_INCLUDE_DIR})

target_link_libraries(andromeda_core PUBLIC
    nlohmann_json::nlohmann_json
    CURL::libcurl
    RtMidi::rtmidi
    glfw
    glad::glad
    imgui::imgui
    OpenGL::GL
    sol2::sol2
    ${LUA_LIBRARIES}
)

if(WIN32)
    target_link_libraries(andromeda_core PUBLIC psapi dbghelp winmm)
endif()

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(andromeda src/main.cpp)
target_link_libraries(andromeda PRIVATE andromeda_core)


if(MSVC)
    target_link_options(andromeda PRIVATE /STACK:8388608)
endif()

if(ANDROMEDA_BUILD_TOOLS)
    add_executable(midi_dump tools/midi_dump.cpp)
    target_link_libraries(midi_dump PRIVATE andromeda_core)

    add_executable(midi_ref_check tools/midi_ref_check.cpp)

    add_executable(editor_check tools/editor_check.cpp)
    target_link_libraries(editor_check PRIVATE andromeda_core)

    add_executable(playback_probe tools/playback_probe.cpp)
    target_link_libraries(playback_probe PRIVATE andromeda_core)

    add_executable(toggle_stress tools/toggle_stress.cpp)
    target_link_libraries(toggle_stress PRIVATE andromeda_core)

    add_executable(timing_check tools/timing_check.cpp)
    target_link_libraries(timing_check PRIVATE andromeda_core)

    add_executable(png_diff tools/png_diff.cpp)
    find_package(Stb REQUIRED)
    target_include_directories(png_diff PRIVATE ${Stb_INCLUDE_DIR})
endif()

add_custom_command(TARGET andromeda POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/assets"
        "$<TARGET_FILE_DIR:andromeda>/assets"
    COMMENT "Copying assets")

# build.rs embedded API_KEY from the environment; do the same.
if(DEFINED ENV{API_KEY})
    target_compile_definitions(andromeda_core PUBLIC EMBEDDED_CLIENT_KEY="$ENV{API_KEY}")
endif()
