#include "editor/editing/data_editing.h"
#include "event_lane.h"

#include <algorithm>
#include <limits>
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

/// Copy of the edit-source fields a build depends on. The build runs across many frames, so it
/// must not read the live DataEditSource, which the user can change in between.
struct LaneSource {
	EventSourceKind kind = EventSourceKind::Velocity;
	std::uint8_t channel = 0xFF;
	std::uint8_t controller = 0;
};

[[nodiscard]] bool event_matches_source(const ChannelEvent& ev, const LaneSource& source) {
	if (!passes_channel_filter(ev.channel, source.channel)) return false;

	bool matches = false;

	std::visit([&](auto&& arg) {
		using T = std::decay_t<decltype(arg)>;
		if constexpr (std::is_same_v<T, NoteAftertouch>) {
			matches = source.kind == EventSourceKind::PolyPressure;
		} else if constexpr (std::is_same_v<T, Controller>) {
			matches = source.kind == EventSourceKind::ControlChange
				&& arg.controller == source.controller;
		} else if constexpr (std::is_same_v<T, ChannelAftertouch>) {
			matches = source.kind == EventSourceKind::ChannelPressure;
		} else if constexpr (std::is_same_v<T, PitchBend>) {
			matches = source.kind == EventSourceKind::PitchBend;
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

/// Same order sort_points() uses: tick, then channel.
[[nodiscard]] bool point_less(const EventLanePoint& a, const EventLanePoint& b) noexcept {
	if (a.tick != b.tick) return a.tick < b.tick;
	return a.channel < b.channel;
}

} // namespace

namespace andromeda::editor {

// ---------------------------------------------------------------------------------------------
// Streamed build
//
// Every track is read through a Cursor that hands out points already ordered by (tick, channel):
// it pulls one run of equal-tick items at a time and sorts just that run. The cursors are merged
// with a heap, so the output is globally ordered as it is produced and never needs the big
// stable_sort the old rebuild ran. Holds (has_next/next_tick) are linked forward as points are
// appended, so there is no second pass either. A build can stop after any point and resume.
// ---------------------------------------------------------------------------------------------
struct EventLane::Job {
	using clock = std::chrono::steady_clock;
	static constexpr std::size_t NPOS = std::numeric_limits<std::size_t>::max();

	struct Cursor {
		std::uint16_t order = 0; ///< position in the request; breaks ties like the old stable sort
		std::uint16_t track = 0;
		std::size_t next = 0;    ///< next unread index of the track's notes/events
		std::vector<EventLanePoint> run;
		std::size_t run_pos = 0;
	};

	enum class Phase { Lane, Ghosts, Done };

	LaneSource src{};
	EventSourceOrigin origin = EventSourceOrigin::Notes;
	double min = 0.0;
	double max = 1.0;
	std::uint16_t lane_track = 0;
	bool want_lane = true;
	bool want_ghosts = true;
	std::vector<std::uint16_t> ghost_tracks;

	Phase phase = Phase::Done;
	bool phase_started = false;
	bool eager = false;       ///< phase filled in one go (tempo), still needs sort + holds
	bool disordered = false;  ///< source wasn't tick-ordered; fall back to one full sort

	std::vector<EventLanePoint> lane_out;
	std::vector<EventLanePoint> ghost_out;
	std::vector<EventLanePoint>* out = nullptr;
	bool track_holds = false;

	std::vector<Cursor> cursors;
	std::vector<std::uint32_t> heap; ///< cursor indices, min-heap on the cursors' head point
	std::vector<std::size_t> last_of_key; ///< index in *out of the latest point per (track, channel)

	std::size_t items_total = 0;
	std::size_t items_done = 0;

	Job(DataEditing& de, const DataEditSource& ds, std::span<const std::uint16_t> ghosts,
	    bool lane, bool ghost_part)
		: want_lane(lane), want_ghosts(ghost_part) {
		src.kind = ds.event_source;
		src.channel = ds.channel;
		src.controller = static_cast<std::uint8_t>(ds.controller);

		const auto* info = EventSources::get_source(src.kind);
		origin = info->info.source_origin;
		min = info->info.range.first;
		max = info->info.range.second;

		lane_track = de.lane_track();
		for (const std::uint16_t t : ghosts) {
			// the active track is never its own ghost: it already is the editable lane
			if (t != lane_track) ghost_tracks.push_back(t);
		}

		// velocity is drawn straight from the notes: the lane carries no points for it
		const bool projects = src.kind != EventSourceKind::Velocity;
		if (projects && want_lane && origin != EventSourceOrigin::Global) items_total += source_size(de, lane_track);
		if (projects && want_ghosts) {
			for (const std::uint16_t t : ghost_tracks) items_total += source_size(de, t);
		}

		phase = want_lane ? Phase::Lane : (want_ghosts ? Phase::Ghosts : Phase::Done);
	}

	std::size_t source_size(DataEditing& de, std::uint16_t track) const {
		if (src.kind == EventSourceKind::Velocity) {
			const auto* notes = de.track_notes(track);
			return notes ? notes->size() : 0;
		}
		const auto* events = de.track_events(track);
		return events ? events->size() : 0;
	}

	/// Reads the next run of equal-tick points of @p c into c.run, ordered by channel.
	/// @return false when the cursor is exhausted.
	bool refill(DataEditing& de, Cursor& c) {
		c.run.clear();
		c.run_pos = 0;

		std::size_t i = c.next;
		if (src.kind == EventSourceKind::Velocity) {
			const auto* notes = de.track_notes(c.track);
			if (notes == nullptr) return false;
			const std::size_t n = notes->size();

			for (; i < n; ++i) {
				const midi::Note& note = (*notes)[i];
				if (!c.run.empty() && note.start != c.run.front().tick) break;
				const double val = (double)(note.get_velocity() - 1) / (126.0);
				c.run.push_back(EventLanePoint{ note.start, static_cast<float>(val),
												note.start + note.length, note.channel, c.track });
			}
		} else {
			const auto* events = de.track_events(c.track);
			if (events == nullptr) return false;
			const std::size_t n = events->size();

			for (; i < n; ++i) {
				const midi::ChannelEvent& event = (*events)[i];
				if (!event_matches_source(event, src)) continue;
				if (!c.run.empty() && event.tick != c.run.front().tick) break;

				double event_val = get_event_value(event);
				event_val = (event_val - min) / (max - min);
				c.run.push_back(EventLanePoint{ event.tick, static_cast<float>(event_val),
												0, event.channel, c.track });
			}
		}

		if (i > c.next) items_done += i - c.next;
		c.next = std::max(i, c.next);

		if (c.run.size() > 1 && !std::is_sorted(c.run.begin(), c.run.end(), point_less)) {
			std::stable_sort(c.run.begin(), c.run.end(), point_less);
		}
		return !c.run.empty();
	}

	[[nodiscard]] const EventLanePoint& head(const Cursor& c) const { return c.run[c.run_pos]; }

	/// heap "less" for std::*_heap: the cursor whose head sorts LATER is "smaller", which makes
	/// the front of the heap the earliest point. Ties fall back to request order.
	[[nodiscard]] bool later(std::uint32_t a, std::uint32_t b) const {
		const EventLanePoint& pa = head(cursors[a]);
		const EventLanePoint& pb = head(cursors[b]);
		if (pa.tick != pb.tick) return pa.tick > pb.tick;
		if (pa.channel != pb.channel) return pa.channel > pb.channel;
		return cursors[a].order > cursors[b].order;
	}

	void add_cursor(std::uint16_t track, std::uint16_t order) {
		Cursor c;
		c.track = track;
		c.order = order;
		cursors.push_back(std::move(c));
	}

	void build_tempo(DataEditing& de) {
		const auto* events = de.global_metas();
		if (events == nullptr) return;

		lane_out.reserve(events->size());
		for (const midi::MetaEvent& event : *events) {
			if (event.event_type != midi::MetaEventType::Tempo) continue;
			const double bpm = 60000000.0 /
				(double)((static_cast<std::uint32_t>(event.data[0]) << 16) |
					(static_cast<std::uint32_t>(event.data[1]) << 8) |
					static_cast<std::uint32_t>(event.data[2]));
			const double point_val = std::clamp((bpm - min) / (max - min), 0.0, 1.0);

			lane_out.push_back(EventLanePoint{ event.tick, static_cast<float>(point_val),
											   0, 0xFF /* 0xFF signifies its a global event */, 0 });
		}
		eager = true;
	}

	void begin_phase(DataEditing& de) {
		phase_started = true;
		eager = false;
		disordered = false;
		cursors.clear();
		heap.clear();
		last_of_key.clear();

		if (phase == Phase::Lane) {
			out = &lane_out;
			// matches the old rebuild: velocity lanes never needed holds, every other source does
			track_holds = src.kind != EventSourceKind::Velocity;
			if (src.kind == EventSourceKind::Velocity) {
				// no points: notes are rendered directly
			} else if (origin != EventSourceOrigin::Global) {
				add_cursor(lane_track, 0);
			} else if (src.kind == EventSourceKind::Tempo) {
				build_tempo(de);
			}
		} else {
			out = &ghost_out;
			track_holds = true;
			for (std::size_t i = 0; src.kind != EventSourceKind::Velocity && i < ghost_tracks.size(); ++i) {
				add_cursor(ghost_tracks[i], static_cast<std::uint16_t>(i));
			}
		}

		// notes map 1:1 to points, so the exact size is known; the contiguous vector then never
		// reallocates (a realloc copy of a multi-GB lane would stall one frame)
		if (src.kind == EventSourceKind::Velocity) {
			std::size_t total = 0;
			for (const Cursor& c : cursors) total += source_size(de, c.track);
			out->reserve(total);
		}

		for (std::uint32_t i = 0; i < cursors.size(); ++i) {
			if (refill(de, cursors[i])) heap.push_back(i);
		}
		std::make_heap(heap.begin(), heap.end(),
			[this](std::uint32_t a, std::uint32_t b) { return later(a, b); });
	}

	void append(const EventLanePoint& p) {
		if (!out->empty() && point_less(p, out->back())) disordered = true;

		if (track_holds) {
			const std::size_t key = (static_cast<std::size_t>(p.track) << 4) | p.channel;
			if (key >= last_of_key.size()) last_of_key.resize(key + 1, NPOS);
			const std::size_t prev = last_of_key[key];
			if (prev != NPOS) {
				(*out)[prev].has_next = true;
				(*out)[prev].next_tick = p.tick;
			}
			last_of_key[key] = out->size();
		}
		out->push_back(p);
	}

	/// @return false when the deadline hit first; the state stays resumable.
	bool drain(DataEditing& de, clock::time_point deadline) {
		const auto cmp = [this](std::uint32_t a, std::uint32_t b) { return later(a, b); };
		std::size_t emitted = 0;

		while (!heap.empty()) {
			std::pop_heap(heap.begin(), heap.end(), cmp);
			const std::uint32_t idx = heap.back();
			Cursor& c = cursors[idx];

			append(c.run[c.run_pos++]);

			if (c.run_pos < c.run.size() || refill(de, c)) {
				std::push_heap(heap.begin(), heap.end(), cmp);
			} else {
				heap.pop_back();
			}

			if ((++emitted & 0x3FFFu) == 0 && clock::now() >= deadline) return false;
		}
		return true;
	}

	void end_phase() {
		if (eager || disordered) {
			// malformed or unsorted source: one blocking sort, then holds from scratch
			EventLane::sort_points(*out);
			if (track_holds) EventLane::compute_holds(*out);
		}
		cursors.clear();
		cursors.shrink_to_fit();
		last_of_key.clear();
		last_of_key.shrink_to_fit();

		phase_started = false;
		phase = (phase == Phase::Lane && want_ghosts) ? Phase::Ghosts : Phase::Done;
	}

	/// @return true when the whole build finished.
	bool run(DataEditing& de, clock::time_point deadline) {
		while (phase != Phase::Done) {
			if (!phase_started) begin_phase(de);
			if (!drain(de, deadline)) return false;
			end_phase();
			if (clock::now() >= deadline && phase != Phase::Done) return false;
		}
		return true;
	}
};

// ---------------------------------------------------------------------------------------------

EventLane::EventLane(DataEditing* data_editing) : _data_editing(data_editing) { }

EventLane::~EventLane() = default;

void EventLane::clear_front(bool lane, bool ghosts) {
	// swap, not clear(): a 100M-point lane must give its memory back
	if (lane) std::vector<EventLanePoint>().swap(lane_points_);
	if (ghosts) std::vector<EventLanePoint>().swap(ghost_points_);
	++revision_;
}

void EventLane::request_rebuild(std::span<const std::uint16_t> ghost_tracks, bool keep_stale) {
	job_.reset();

	const DataEditSource* ds = _data_editing ? _data_editing->get_edit_source() : nullptr;
	if (ds == nullptr) {
		clear_front(true, true);
		return;
	}
	if (!keep_stale) {
		clear_front(true, true);
	}
	job_ = std::make_unique<Job>(*_data_editing, *ds, ghost_tracks, true, true);
}

void EventLane::cancel_rebuild() noexcept { job_.reset(); }

float EventLane::progress() const noexcept {
	if (!job_) return 1.0f;
	if (job_->items_total == 0) return 0.0f;
	return std::min(1.0f, static_cast<float>(static_cast<double>(job_->items_done) /
	                                         static_cast<double>(job_->items_total)));
}

bool EventLane::pump(std::chrono::microseconds budget) {
	if (!job_) return false;
	if (_data_editing == nullptr) {
		job_.reset();
		return false;
	}

	const auto deadline = Job::clock::now() + budget;
	if (!job_->run(*_data_editing, deadline)) return true;

	commit();
	return false;
}

void EventLane::commit() {
	// moving over the front frees the previous lists
	if (job_->want_lane) lane_points_ = std::move(job_->lane_out);
	if (job_->want_ghosts) ghost_points_ = std::move(job_->ghost_out);

	// trim only small lanes; shrinking a huge one would copy it
	const auto trim = [](std::vector<EventLanePoint>& v) {
		if (v.size() < (1u << 22) && v.capacity() > v.size()) v.shrink_to_fit();
	};
	trim(lane_points_);
	trim(ghost_points_);

	job_.reset();
	++revision_;
}

void EventLane::run_to_completion(bool lane, bool ghosts,
                                  std::span<const std::uint16_t> ghost_tracks) {
	job_.reset();

	const DataEditSource* ds = _data_editing ? _data_editing->get_edit_source() : nullptr;
	if (ds == nullptr) {
		clear_front(lane, ghosts);
		return;
	}

	job_ = std::make_unique<Job>(*_data_editing, *ds, ghost_tracks, lane, ghosts);
	job_->run(*_data_editing, Job::clock::time_point::max());
	commit();
}

void EventLane::rebuild_lane() {
	run_to_completion(true, false, {});
}

void EventLane::rebuild_ghosts(std::span<const std::uint16_t> tracks) {
	run_to_completion(false, true, tracks);
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