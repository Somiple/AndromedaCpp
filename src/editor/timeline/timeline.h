#pragma once

#include <memory>
#include <vector>
#include "editor/util.h"
#include "midi/events/meta_event.h"

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
};

class Timeline {
public:
	void rebuild(const std::vector<MetaEvent>& metas, uint16_t ppq);

	// time
	[[nodiscard]] double   tick_to_secs(MIDITick tick) const;
	[[nodiscard]] MIDITick secs_to_tick(double secs) const;
	[[nodiscard]] double   bpm_at(MIDITick tick) const;

	// musical position
	[[nodiscard]] BarPos   bar_at(MIDITick tick) const;
	// [[nodiscard]] MIDITick bar_start(std::uint32_t bar) const;

	// snapping: `step` is in ticks, the grid is anchored to the start of the current bar
	[[nodiscard]] MIDITick snap_floor(MIDITick tick, MIDITick step) const;
	[[nodiscard]] MIDITick snap_ceil(MIDITick tick, MIDITick step) const;
	[[nodiscard]] MIDITick snap_nearest(MIDITick tick, MIDITick step) const;

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
private:
	std::uint16_t ppq_ = 960;
	std::vector<TempoPoint>  tempos_;
	std::vector<TimeSigPoint> sigs_;
};

}