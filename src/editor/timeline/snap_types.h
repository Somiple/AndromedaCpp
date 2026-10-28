#pragma once

// ---------------------------------------------------------------------------
// Grid-snap vocabulary shared by the timeline, the renderers and the UI.
//
// Split out of `timeline.h` so that headers which only need to *name* a snap
// value (glyph rendering, panels, navigation) do not have to include the whole
// `Timeline` class and its tempo/time-signature vectors. `timeline.h` includes
// this header, so existing include sites keep compiling unchanged.
// ---------------------------------------------------------------------------

namespace andromeda::editor::timeline {

/// Extra subdivision applied on top of the base grid (triplet, dotted, ...).
enum class SnapModifier { Straight, Triplet, Dotted, Quintuplet, Septuplet };

/// The base grid a snapping operation lands on.
enum class TimelineGridSnap {
	Bar,
	Beat,
	HalfBeat,
	QuarterBeat,
	WholeNote,
	HalfNote,
	ThirdNote,
	QuarterNote,
	EighthNote,
	SixteenthNote,
	Note32,
	Note64,
	Note128,
	None
};

} // namespace andromeda::editor::timeline
