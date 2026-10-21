#include "event_producer.h"

#include <algorithm>
#include <chrono>
#include <queue>
#include <type_traits>
#include <variant>
#include <vector>

#include "app/main_window.h"

namespace andromeda::audio {

using namespace std::chrono_literals;

namespace {

struct HeapItem {
	ScheduledEvent ev{};
	int source = -1;   // cursor to refill from, or -1 for pending note-offs
};

struct HeapCmp {
	bool operator()(const HeapItem& a, const HeapItem& b) const {
		if (a.ev.time_ns != b.ev.time_ns) return a.ev.time_ns > b.ev.time_ns;
		return a.ev.priority > b.ev.priority;
	}
};

}

void EventProducer::start(std::int64_t start_tick, double start_secs) {
	stop();
	_stop = false;
	_done = false;
	_ring.clear();
	_thread = std::thread([=, this] { run(start_tick, start_secs); });
}

void EventProducer::stop() {
	_stop = true;
	if (_thread.joinable()) _thread.join();
}

void EventProducer::run(std::int64_t start_tick, double start_secs) {
	auto* project_manager = _app->editor_controller.get_project_manager();
	auto* timeline = project_manager->get_timeline();
	// TODO: still unsynchronised with the UI (see the mutex TODO in ProjectData)
	const auto& tracks = *project_manager->get_tracks();

	// ticks -> ns relative to start_secs; never negative, so rounding at the start point can't
	// produce an event "before" playback began
	auto to_ns = [&](std::int64_t tick) -> std::int64_t {
		const double ns = (timeline->tick_to_secs(tick) - start_secs) * 1e9;
		return ns > 0.0 ? static_cast<std::int64_t>(ns) : 0;
	};

	// one cursor per sorted source: (track, notes) and (track, channel events)
	struct Cursor {
		const midi::MIDITrack* trk;
		bool is_notes;
		std::size_t idx;
	};
	std::vector<Cursor> cursors;
	cursors.reserve(tracks.size() * 2);
	for (const auto& t : tracks) {
		cursors.push_back({ &t, true, 0 });
		cursors.push_back({ &t, false, 0 });
	}

	// next event of a cursor (notes before start_tick are skipped)
	auto next_from = [&](int ci, HeapItem& out) -> bool {
		Cursor& c = cursors[ci];

		if (c.is_notes) {
			const auto& notes = c.trk->get_notes();
			while (c.idx < notes.size() && static_cast<std::int64_t>(notes[c.idx].start) < start_tick) ++c.idx;
			if (c.idx >= notes.size()) return false;

			const auto& n = notes[c.idx++];
			out = { { to_ns(n.start), pack_midi(0x90 | n.channel, n.key, n.velocity), 2 }, ci };
			return true;
		}

		const auto& evs = c.trk->get_channel_evs();
		while (c.idx < evs.size()) {
			const auto& e = evs[c.idx++];

			std::uint32_t packed = 0;
			std::visit([&](auto&& arg) {
				using T = std::decay_t<decltype(arg)>;

				if constexpr (std::is_same_v<T, midi::NoteAftertouch>) {
					packed = pack_midi(0xA0 | e.channel, arg.key, arg.pressure);
				} else if constexpr (std::is_same_v<T, midi::Controller>) {
					packed = pack_midi(0xB0 | e.channel, arg.controller, arg.value);
				} else if constexpr (std::is_same_v<T, midi::ProgramChange>) {
					packed = pack_midi(0xC0 | e.channel, arg.program, 0x00);
				} else if constexpr (std::is_same_v<T, midi::ChannelAftertouch>) {
					packed = pack_midi(0xD0 | e.channel, arg.amount, 0x00);
				} else if constexpr (std::is_same_v<T, midi::PitchBend>) {
					packed = pack_midi(0xE0 | e.channel, arg.lsb, arg.msb);
				}
			}, e.event_type);
			if (!packed) continue;   // a type we don't send (sysex etc.)

			// events before the start point still fire, at t=0, so program changes, CCs and
			// bends are in effect when you start mid-song
			const std::int64_t t = static_cast<std::int64_t>(e.tick) < start_tick ? 0 : to_ns(e.tick);
			out = { { t, packed, 1 }, ci };
			return true;
		}
		return false;
	};

	std::priority_queue<HeapItem, std::vector<HeapItem>, HeapCmp> heap;
	for (int i = 0; i < static_cast<int>(cursors.size()); ++i) {
		HeapItem it{};
		if (next_from(i, it)) heap.push(it);
	}

	while (!heap.empty() && !_stop) {
		const HeapItem top = heap.top();
		heap.pop();

		// a note-on schedules its own note-off
		if (top.ev.priority == 2) {
			const Cursor& c = cursors[top.source];
			const auto& n = c.trk->get_notes()[c.idx - 1];   // the cursor is only refilled below
			const std::int64_t off_tick = static_cast<std::int64_t>(n.start) + static_cast<std::int64_t>(n.length);
			heap.push({ { to_ns(off_tick), pack_midi(0x80 | n.channel, n.key, 0), 0 }, -1 });
		}

		// backpressure: wait for the consumer to catch up
		while (!_ring.push(top.ev)) {
			if (_stop) return;
			std::this_thread::sleep_for(1ms);
		}

		if (top.source >= 0) {
			HeapItem next{};
			if (next_from(top.source, next)) heap.push(next);
		}
	}

	if (!_stop) _done.store(true, std::memory_order_release);
}

}