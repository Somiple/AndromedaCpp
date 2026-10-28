#pragma once

#include <imgui.h>

// Only the snap vocabulary is needed here, not the whole Timeline class.
#include "editor/timeline/snap_types.h"

// ---------------------------------------------------------------------------
// Music notation glyph rendering.
//
// Extracted from `app/ui/panels.cpp`, which carried ~270 lines of note/measure
// drawing primitives inside an anonymous namespace. They are independent of any
// panel state, so they now form a reusable module: any future panel that needs a
// musical icon (snap selector, quantise menu, toolbar, ...) can draw it here.
// The drawing code itself is unchanged.
// ---------------------------------------------------------------------------

namespace andromeda::app::glyphs {

/// A single drawable notation symbol: a notehead with flags/modifiers or a measure block.
struct Glyph {
    enum class Kind { Blank, Note, Measure } kind = Kind::Blank;
    // note
    bool hollow = false;    // half / whole
    bool whole  = false;    // no stem
    int  flags  = 0;        // 1 = eighth ... 5 = 128th
    bool dotted = false;
    char tuplet = 0;        // '3', '5', '7'
    // measure
    float portion = 1.0f;   // fraction of the measure that is highlighted
    bool  ticks   = false;  // beat dividers
};

/// Build a note glyph with the given number of flags.
Glyph make_note(int flags, bool hollow = false, bool whole = false);

/// Build a measure glyph; `portion` is the highlighted fraction, `ticks` adds beat dividers.
Glyph make_measure(float portion, bool ticks);

/// Draw `g` at `origin` with symbol size `s`.
void draw_glyph(ImDrawList* dl, ImVec2 origin, float s, ImU32 col, const Glyph& g);

/// Glyph that represents a grid-snap division.
Glyph glyph_for(editor::timeline::TimelineGridSnap s);

/// Glyph that represents a snap modifier (dotted / tuplet).
Glyph glyph_for(editor::timeline::SnapModifier m);

/// Selectable row that draws a glyph followed by `text`.
bool icon_selectable(const char* text, bool selected, const Glyph& g);

/// Human-readable name of a grid-snap division.
const char* snap_name(editor::timeline::TimelineGridSnap s);

/// Human-readable name of a snap modifier (empty string for `Straight`).
const char* modifier_name(editor::timeline::SnapModifier m);

/// Glyph-driven radio entry: calls `set(value)` when the row is clicked.
template <typename E, typename Get, typename Set>
void choice(const char* label, E value, Get get, Set set) {
    if (icon_selectable(label, get() == value, glyph_for(value))) set(value);
}

} // namespace andromeda::app::glyphs
