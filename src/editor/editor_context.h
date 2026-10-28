#pragma once

#include <cstdint>

#include "core/math/vector2.h"
#include "core/shared.h"
#include "editor/navigation.h"
#include "editor/playback_provider.h"
#include "editor/tick_view.h"

namespace andromeda::editor {

/// Keyboard modifier snapshot for the current frame.
/// REFACTOR: moved here from ui/app/editor_context.h together with the context that holds
/// it, so the editor layer does not have to include a ui/ header to name it.
struct KeyModifierState {
    bool shift = false; ///< True while shift is held.
    bool ctrl = false;  ///< True while ctrl is held.
    bool alt = false;   ///< True while alt is held.
};

/// Which MIDI lane the data view shows.
/// REFACTOR: mirrors the three states of the UI's VS_PianoRoll_DataViewState without the
/// editor layer having to name a ui/ enum. The bridge maps one to the other.
enum class DataViewState { Hidden, NoteVelocities, PitchBend };

/// Flat, editor-owned mirror of the UI view settings that editor code may read.
///
/// REFACTOR: `EditorContext` used to hold `util::SharedMutPtr<ViewSettings>*`, but
/// `ViewSettings` lives in ui/app/view_settings.h and carries UI-only enums. Replacing the
/// pointer with this value snapshot removes the last ui/ dependency from the editor layer.
/// It is refreshed once per frame by the UI host (see app::sync_view_snapshot), so reading
/// a field costs nothing and there is no locking or lifetime question.
struct ViewSnapshot {
    float dataview_size = 200.0f;      ///< Height of the MIDI data view lane, in pixels.
    std::uint16_t current_track = 0;   ///< Track the editor is currently focused on.
    bool autoscroll = true;            ///< Whether the view follows the playhead.
    bool show_meta_events = false;     ///< Whether meta events are drawn.
    DataViewState dataview_state = DataViewState::NoteVelocities; ///< Which lane is shown.
    bool track_view_active = false;    ///< True when the track view (not the piano roll) is shown.
};

/// Narrow slice of application state that editor components are allowed to read.
///
/// Editor components used to take a whole `MainWindow*` and reach through it for a handful
/// of values. They now take this bundle instead, which is a plain aggregate of the exact
/// inputs they need and nothing else.
///
/// Long-lived objects are referenced through stable pointers owned elsewhere; per-frame
/// values (mouse, keys, tick view, view snapshot) are refreshed by the host before the
/// components run.
struct EditorContext {
    // --- per-frame input, refreshed by the host before components update ---
    const util::math::Vector2<float>* mouse_pos = nullptr; ///< Cursor position in imgui units.
    const KeyModifierState* key_modifiers = nullptr;       ///< Modifier keys of this frame.
    const bool* mouse_over_ui = nullptr;                   ///< True when the cursor is over UI chrome.
    TickView central_tick_view{};                          ///< Tick view of the central editor.

    // --- long-lived editor state, owned by the application ---
    util::SharedPtr<PianoRollNavigation>* nav = nullptr;      ///< Piano roll navigation.
    util::SharedPtr<TrackViewNavigation>* track_nav = nullptr; ///< Track view navigation.

    // --- narrow services ---
    IPlaybackProvider* playback = nullptr; ///< Transport access; owned by the ui layer.
    ViewSnapshot view{};                   ///< Value mirror of the UI view settings.
};

} // namespace andromeda::editor
