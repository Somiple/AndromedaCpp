#pragma once

#include <atomic>
#include <memory>
#include <vector>
#include "editor/util.h"
#include "editor/timeline/snap_types.h"
#include "midi/events/meta_event.h"
#include <utility>

namespace andromeda::editor::timeline {

using midi::MIDITick;

struct TempoPoint {
	MIDITick tick = 0;
	double bpm = 120.0;
	double secs_at_tick = 0.0;
};

struct TimeSigPoint {
	MIDITick tick = 0;
	uint8_t numerator = 4;
	uint8_t denominator = 4;
	MIDITick beat_len = 0;
	MIDITick bar_len = 0;
	uint32_t first_bar = 0;
};

struct BarPos {
	uint32_t bar;
	MIDITick start;
	MIDITick len;
	MIDITick beat_len;
	std::pair<uint8_t, uint8_t> time_sig; // time signature at this bar
};

struct TickRatio {
	std::uint64_t num = 1;
	std::uint64_t den = 1;
};

// SnapModifier / TimelineGridSnap are declared in `snap_types.h` above, so the
// grid vocabulary can be included without dragging in the whole Timeline.

class Timeline {
public:
	void rebuild(const std::vector<MetaEvent>& metas, uint16_t ppq);

	// time
	[[nodiscard]] double   tick_to_secs(MIDITick tick) const;
	[[nodiscard]] MIDITick secs_to_tick(double secs) const;
	[[nodiscard]] double   bpm_at(MIDITick tick) const;

	// musical position
	[[nodiscard]] BarPos   bar_at(MIDITick tick) const;

#pragma region grid snapping
	void set_snap(TimelineGridSnap snap) { snap_ = snap; }
	void set_snap_modifier(SnapModifier mod) { modifier_ = mod; }
	[[nodiscard]] TimelineGridSnap get_snap() const { return snap_; }
	[[nodiscard]] SnapModifier get_snap_modifier() const { return modifier_; }

	// snapping: `step` is in ticks, the grid is anchored to the start of the current bar
	[[nodiscard]] MIDITick snap_floor(MIDITick tick, TickRatio s) const;
	[[nodiscard]] MIDITick snap_ceil(MIDITick tick, TickRatio s) const;
	[[nodiscard]] MIDITick snap_nearest(MIDITick tick, TickRatio s) const;

	// functions that tools call
	[[nodiscard]] MIDITick snap_floor(MIDITick tick) const { return snap_floor(tick, grid_at(tick)); }
	[[nodiscard]] MIDITick snap_ceil(MIDITick tick) const { return snap_ceil(tick, grid_at(tick)); }
	[[nodiscard]] MIDITick snap_nearest(MIDITick tick) const { return snap_nearest(tick, grid_at(tick)); }

	[[nodiscard]] MIDITick snap_floor(MIDITick tick, TimelineGridSnap snap) const { return snap_floor(tick, grid_at(tick, snap)); }
	[[nodiscard]] MIDITick snap_ceil(MIDITick tick, TimelineGridSnap snap) const { return snap_ceil(tick, grid_at(tick, snap)); }
	[[nodiscard]] MIDITick snap_nearest(MIDITick tick, TimelineGridSnap snap) const { return snap_nearest(tick, grid_at(tick, snap)); }

	// length of one grid step at `tick`, in ticks (>= 1): default note length, nudge amount...
	MIDITick step_length_at(MIDITick tick, TimelineGridSnap snap) const;
	MIDITick step_length_at(MIDITick tick) const;
#pragma endregion

	template <class F>
	void for_each_time_signature(MIDITick from, MIDITick to, F&& fn) const {
		auto it = std::partition_point(sigs_.begin(), sigs_.end(),
			[&](const TimeSigPoint& s) { return s.tick <= from; });

		if (it != sigs_.begin()) --it;

		for (; it != sigs_.end() && it->tick <= to; ++it) {
			const auto next = it + 1;
			fn(*it, next != sigs_.end() ? std::optional<MIDITick>{next->tick} : std::nullopt);
		}
	}

	[[nodiscard]] const std::vector<TimeSigPoint>& get_time_signatures() const { return sigs_; }

	// other functions
	[[nodiscard]] std::uint64_t get_revision() const { return revision_.load(std::memory_order_relaxed); }
private:
	TickRatio grid_at(MIDITick tick) const;   // builds the ratio for the bar containing `tick`
	TickRatio grid_at(MIDITick tick, TimelineGridSnap snap) const;

	void bump_revision() { revision_.fetch_add(1, std::memory_order_relaxed); }

	TimelineGridSnap snap_ = TimelineGridSnap::QuarterNote;
	SnapModifier modifier_ = SnapModifier::Straight;

	std::uint16_t ppq_ = 960;
	std::vector<TempoPoint>  tempos_;
	std::vector<TimeSigPoint> sigs_;

	std::atomic<std::uint64_t> revision_{ 0 };

};

}