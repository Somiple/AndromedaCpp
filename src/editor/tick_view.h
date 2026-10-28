#pragma once

namespace andromeda::editor {

// REFACTOR: single canonical definition. This struct used to be declared twice in
// namespace andromeda::app — once in ui/app/editor_context.h and once in
// editor/editor_widget.h — which is an ODR violation the moment both headers are
// included by one translation unit. The editor layer owns it because it is domain
// data; ui/app/editor_context.h and editor/editor_widget.h re-export the name.
//
// Smoothed left edge and width of a view, in ticks.
struct TickView {
    float pos = 0.0f;  ///< Leftmost visible tick position.
    float zoom = 0.0f; ///< Number of ticks the view spans horizontally.
};

} // namespace andromeda::editor
