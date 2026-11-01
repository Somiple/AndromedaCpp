#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include "editor/midi_types.h"

namespace andromeda::editor {

class DataEditing;

/// One projected point of the data lane.
///
/// refactor: the lane used to be a bare (tick, value) pair, which could not express a note's real
/// length, could not tell the renderer which palette colour to use, and could not keep onion
/// ghosts apart from the editable points. The new members are appended with defaults, so every
/// existing `EventLanePoint{ tick, value }` initialiser keeps its previous meaning.
struct EventLanePoint {
	editor::MIDITick tick = 0;     ///< start position in ticks
	float value = 0.0f;            ///< value in the source's own units
	editor::MIDITick end_tick = 0; ///< exact end tick for note sources; 0 when there is no length
	std::uint8_t channel = 0;      ///< MIDI channel the event belongs to
	std::uint16_t track = 0;       ///< owning track, so the UI can resolve a track/channel colour

	editor::MIDITick next_tick = 0;
	bool has_next = false;

	/// @return the note length in ticks; 0 when this point carries no duration.
	[[nodiscard]] editor::MIDITick length() const noexcept {
		return end_tick > tick ? end_tick - tick : 0;
	}

	/// @return true when the point has a real duration, which only per-note sources do.
	[[nodiscard]] bool carries_length() const noexcept { return end_tick > tick; }
};

/// The lanes the data editor draws: one editable primary lane plus read-only ghost lanes.
///
/// Colour-free and UI-free by construction: points carry (track, channel) and the renderer turns
/// that into a palette colour, so the editor layer never learns about ImGui or the note palette.
class EventLane {
public:
	/// @param data_editing owner of the source and track state this lane projects; not owned.
	explicit EventLane(DataEditing* data_editing)
		: _data_editing(data_editing) { }

	/// Rebuilds the editable lane from the current edit source and active track.
	void rebuild_lane();

	/// Rebuilds the read-only ghost lanes from the given tracks.
	/// @param tracks onion tracks chosen by the view settings; the active track is skipped.
	void rebuild_ghosts(std::span<const std::uint16_t> tracks);

	/// @return the editable points: one per note for the velocity source, one per event otherwise.
	[[nodiscard]] const std::vector<EventLanePoint>& points() const noexcept {
		return lane_points_;
	}

	/// @return the dimmed, never-editable points projected from the onion tracks.
	[[nodiscard]] const std::vector<EventLanePoint>& ghosts() const noexcept {
		return ghost_points_;
	}

	/// @return bumped after every rebuild, so a cached renderer can tell the points changed.
	[[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
	/// Appends the points of one track to @p out.
	/// @param track track to project.
	/// @param channel_filter channel to keep; 0xFF keeps every channel.
	/// @param out destination list, appended to and ordered by the caller.
	void rebuild_track_into(std::uint16_t track, std::uint8_t channel_filter,
	                        std::vector<EventLanePoint>& out) const;

	static void compute_holds(std::vector<EventLanePoint>& points);

	/// @param points list to order in place.
	static void sort_points(std::vector<EventLanePoint>& points);

	std::vector<EventLanePoint> lane_points_{};
	std::vector<EventLanePoint> ghost_points_{};
	std::uint64_t revision_ = 0;

	DataEditing* _data_editing;
};

} // namespace andromeda::editor
