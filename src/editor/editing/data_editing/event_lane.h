#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
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
///
/// Rebuilds are streamed. request_rebuild() snapshots what to project and pump() produces points
/// in small time slices into a back buffer; the front lists (points()/ghosts()) only change when
/// the whole build commits, so readers never see a half-built lane and revision() bumps once.
class EventLane {
public:
	/// @param data_editing owner of the source and track state this lane projects; not owned.
	explicit EventLane(DataEditing* data_editing);
	~EventLane();

	EventLane(const EventLane&) = delete;
	EventLane& operator=(const EventLane&) = delete;

	// ---- streamed rebuild ---------------------------------------------------------------------

	/// Starts (or restarts) a streamed rebuild of the lane and its ghosts. Any build still in
	/// flight is dropped. Nothing is produced until pump() is called.
	/// @param ghost_tracks onion tracks; the active track is skipped.
	/// @param keep_stale true keeps the current points readable until the new ones commit (use it
	///        when only the data changed); false clears them now, so a lane of a different
	///        source/track is never shown as if it were the new one, and its memory is freed.
	void request_rebuild(std::span<const std::uint16_t> ghost_tracks, bool keep_stale);

	/// Produces points until @p budget is spent or the build finishes. Call once per frame, only
	/// after the caller has confirmed the request is still current (edits invalidate it).
	/// @return true while work remains; false when idle or when this call committed the result.
	bool pump(std::chrono::microseconds budget);

	/// Drops the in-flight build, keeping whatever the front lists hold.
	void cancel_rebuild() noexcept;

	[[nodiscard]] bool building() const noexcept { return job_ != nullptr; }

	/// @return 0..1 share of source items consumed by the in-flight build; 1 when idle.
	[[nodiscard]] float progress() const noexcept;

	// ---- synchronous rebuild (kept for callers that need the result immediately) --------------

	/// Rebuilds the editable lane from the current edit source and active track, to completion.
	void rebuild_lane();

	/// Rebuilds the read-only ghost lanes from the given tracks, to completion.
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

	/// @return bumped whenever the front lists change, so a cached renderer can tell.
	[[nodiscard]] std::uint64_t revision() const noexcept { return revision_; }

private:
	struct Job; ///< resumable build state, defined in the .cpp

	void run_to_completion(bool lane, bool ghosts, std::span<const std::uint16_t> ghost_tracks);
	void commit();
	void clear_front(bool lane, bool ghosts);

	static void compute_holds(std::vector<EventLanePoint>& points);

	/// @param points list to order in place.
	static void sort_points(std::vector<EventLanePoint>& points);

	std::vector<EventLanePoint> lane_points_{};
	std::vector<EventLanePoint> ghost_points_{};
	std::uint64_t revision_ = 0;

	std::unique_ptr<Job> job_;
	DataEditing* _data_editing;
};

} // namespace andromeda::editor