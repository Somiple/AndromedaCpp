#pragma once

#include "editor/editor_context.h"
#include "ui/app/view_settings.h"

namespace andromeda::app {

/// Copies the UI-owned ViewSettings into the editor-owned value snapshot.
///
/// REFACTOR: this is the single place where ViewSettings is translated into the editor
/// layer's ViewSnapshot. Keeping the mapping in one inline function means the editor never
/// needs to know about ViewSettings' UI-only enums, and the conversion stays allocation-free
/// (one small struct assignment per frame).
///
/// @param s Live view settings owned by the window.
/// @return A plain value snapshot with the fields editor components may read.
inline editor::ViewSnapshot make_view_snapshot(const ViewSettings& s, std::uint16_t active_track) {
    editor::ViewSnapshot out;
    out.dataview_size = s.pr_dataview_size;
    // refactor: the selection is owned by EditorController, so it is passed in here rather
    // than read from ViewSettings, which no longer stores it.
    out.current_track = active_track;
    out.autoscroll = s.pr_autoscroll;
    out.show_meta_events = s.show_meta_events;
    // The editor layer has no ViewSettings enum: map the three-state dataview selector onto
    // the editor-owned enum so components never name a ui/ type.
    switch (s.pr_dataview_state) {
    case VS_PianoRoll_DataViewState::Hidden:
        out.dataview_state = editor::DataViewState::Hidden;
        break;
    case VS_PianoRoll_DataViewState::NoteVelocities:
        out.dataview_state = editor::DataViewState::NoteVelocities;
        break;
    case VS_PianoRoll_DataViewState::PitchBend:
        out.dataview_state = editor::DataViewState::PitchBend;
        break;
    }
    return out;
}

/// Refreshes @p ctx's view snapshot from the live UI settings. Call once per frame, before
/// any editor component runs.
///
/// @param ctx Context whose `view` member is overwritten.
/// @param s   Live view settings owned by the window.
inline void sync_view_snapshot(editor::EditorContext& ctx, const ViewSettings& s,
                               std::uint16_t active_track) {
    ctx.view = make_view_snapshot(s, active_track);
}

} // namespace andromeda::app
