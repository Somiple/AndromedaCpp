#include "editor/editing/data_editing.h"
#include "event_lane.h"

#include <algorithm>
#include "core/debugger.h"

namespace {

/// @param channel MIDI channel of the event.
/// @param filter channel to keep; 0xFF keeps every channel.
/// @return true when the event passes the filter.
[[nodiscard]] bool passes_channel_filter(std::uint8_t channel, std::uint8_t filter) noexcept {
	return filter == 0xFF || channel == filter;
}

using namespace andromeda::midi;
using namespace andromeda::editor;

[[nodiscard]] bool event_matches_source(const ChannelEvent& ev, const DataEditSource* source) {
	if (!passes_channel_filter(ev.channel, source->channel)) return false;

	bool matches = false;

	std::visit([&](auto&& arg) {
		using T = std::decay_t<decltype(arg)>;
		if constexpr (std::is_same_v<T, NoteAftertouch>) {
			matches = source->event_source == EventSourceKind::PolyPressure;
		} else if constexpr (std::is_same_v<T, Controller>) {
			matches = source->event_source == EventSourceKind::ControlChange
				&& arg.controller == source->controller;
		} else if constexpr (std::is_same_v<T, ChannelAftertouch>) {
			matches = source->event_source == EventSourceKind::ChannelPressure;
		} else if constexpr (std::is_same_v<T, PitchBend>) {
			matches = source->event_source == EventSourceKind::PitchBend;
		} else {
			matches = false;
		}
	}, ev.event_type);

	return matches;
}

[[nodiscard]] double get_event_value(const ChannelEvent& ev) {
	double val = 0.0;

	std::visit([&](auto&& arg) {
		using T = std::decay_t<decltype(arg)>;
		if constexpr (std::is_same_v<T, NoteAftertouch>) {
			val = arg.pressure;
		} else if constexpr (std::is_same_v<T, Controller>) {
			val = arg.value;
		} else if constexpr (std::is_same_v<T, ChannelAftertouch>) {
			val = arg.amount;
		} else if constexpr (std::is_same_v<T, PitchBend>) {
			val = (double)(static_cast<std::size_t>(arg.lsb) |
						  (static_cast<std::size_t>(arg.msb) << 7));
			val -= 8192;
		}
	}, ev.event_type);

	return val;
}

} // namespace

namespace andromeda::editor {

void EventLane::rebuild_lane() {
	lane_points_.clear();

	if (_data_editing == nullptr) {
		++revision_;
		return;
	}

	const DataEditSource* data_source = _data_editing->get_edit_source();
	if (data_source == nullptr) {
		++revision_;
		return;
	}

	const auto* source_info = editor::EventSources::get_source(data_source->event_source);
	if (source_info->info.source_origin != EventSourceOrigin::Global) {
		rebuild_track_into(_data_editing->lane_track(), data_source->channel, lane_points_);
	} else if (data_source->event_source == EventSourceKind::Tempo) {
		const auto [min, max] = source_info->info.range;
		const auto* events = _data_editing->global_metas();
		lane_points_.reserve(events->size());

		for (const midi::MetaEvent& event : *events) {
			if (event.event_type != midi::MetaEventType::Tempo) continue;
			const double bpm = 60000000.0 /
				(double)((static_cast<std::uint32_t>(event.data[0]) << 16) |
					(static_cast<std::uint32_t>(event.data[1]) << 8) |
					static_cast<std::uint32_t>(event.data[2]));
			const double point_val = std::clamp((bpm - min) / (max - min), 0.0, 1.0);

			lane_points_.push_back(EventLanePoint{ event.tick,
										  static_cast<float>(point_val),
										  0, 0xFF /* 0xFF signifies its a global event */, 0});
		}
	}
	lane_points_.shrink_to_fit();
	sort_points(lane_points_);
	compute_holds(lane_points_);

	++revision_;
}

void EventLane::rebuild_ghosts(std::span<const std::uint16_t> tracks) {
	ghost_points_.clear();

	if (_data_editing == nullptr) {
		++revision_;
		return;
	}

	const DataEditSource* data_source = _data_editing->get_edit_source();
	if (data_source == nullptr) {
		++revision_;
		return;
	}

	const std::uint16_t active_track = _data_editing->lane_track();
	for (const std::uint16_t track : tracks) {
		// the active track is never its own ghost: it already is the editable lane
		if (track == active_track) {
			continue;
		}
		rebuild_track_into(track, data_source->channel, ghost_points_);
	}
	sort_points(ghost_points_);
	compute_holds(ghost_points_);

	++revision_;
}

void EventLane::rebuild_track_into(std::uint16_t track, std::uint8_t channel_filter,
                                   std::vector<EventLanePoint>& out) const {
	const std::vector<midi::Note>* notes = _data_editing->track_notes(track);
	if (notes == nullptr) {
		return;
	}

	const DataEditSource* data_source = _data_editing->get_edit_source();

	if (data_source == nullptr) {
		return;
	}

	const auto* source_info = editor::EventSources::get_source(data_source->event_source);
	const auto [min, max] = source_info->info.range;

	switch (data_source->event_source) {
	case EventSourceKind::Velocity: {
		// Data viewer can draw velocities as normal
		return;
	}
	default: {
		const auto* events = _data_editing->track_events(track);
		out.reserve(out.size() + events->size());

		for (const midi::ChannelEvent& event : *events) {
			if (!event_matches_source(event, data_source)) continue;

			double event_val = get_event_value(event);
			event_val = (event_val - (double)min) / (double)(max - min);

			out.push_back(EventLanePoint{ event.tick,
										   static_cast<float>(event_val),
										   0, event.channel, track });
		}

		break;
	}
	}
}

void EventLane::compute_holds(std::vector<EventLanePoint>& points) {
	std::unordered_map<std::uint32_t, MIDITick> next;
	for (std::size_t i = points.size(); i-- > 0;) {
		EventLanePoint& p = points[i];
		const std::uint32_t key = (static_cast<std::uint32_t>(p.track) << 4) | p.channel;
		const auto it = next.find(key);
		p.has_next = it != next.end();
		p.next_tick = p.has_next ? it->second : 0;
		next[key] = p.tick;
	}
}

void EventLane::sort_points(std::vector<EventLanePoint>& points) {
	std::stable_sort(points.begin(), points.end(),
	                 [](const EventLanePoint& lhs, const EventLanePoint& rhs) {
		                 if (lhs.tick != rhs.tick) {
			                 return lhs.tick < rhs.tick;
		                 }
		                 return lhs.channel < rhs.channel;
	                 });
}

} // namespace andromeda::editor
