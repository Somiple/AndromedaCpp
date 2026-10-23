#include <bitset>
#include <chrono>
#include <vector>
#include "util/util.h"

#if defined(_WIN32)
#include <windows.h>
#define LIBRARY_NAME "maestro.dll"
#define open_library() LoadLibraryA(LIBRARY_NAME)
#define find_symbol(lib, name) ((void *)GetProcAddress((lib), (name)))
typedef HMODULE Library;

#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#else
#include <dlfcn.h>
#if defined(__APPLE__)
#define LIBRARY_NAME "libmaestro.dylib"
#else
#define LIBRARY_NAME "libmaestro.so"
#endif
#define open_library() dlopen(LIBRARY_NAME, RTLD_NOW | RTLD_LOCAL)
#define find_symbol(lib, name) dlsym((lib), (name))
typedef void* Library;
#endif

#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define CPU_RELAX() _mm_pause()
#else
#define CPU_RELAX() ((void)0)
#endif

#include "maestro_engine.h"
#include "util/debugger.h"
#include "app/main_window.h"
#include "app/editor_app.h"
#include "app/managers.h"

namespace {
using andromeda::util::Debugger;

using clock_t_ = std::chrono::steady_clock;
using namespace std::chrono_literals;

std::uint32_t pack_message(std::uint8_t status, std::uint8_t channel, std::uint8_t b1, std::uint8_t b2) {
	return static_cast<std::uint32_t>(status | (channel & 0x0F)) |
		   (static_cast<std::uint32_t>(b1) << 8) |
		   (static_cast<std::uint32_t>(b2) << 16);
}

constexpr int64_t NS_LIMIT = std::numeric_limits<int64_t>::max() / 4;

// compile-time stuff
constexpr int64_t JUMP_NS = 15'000'000;
constexpr int64_t LATENCY_NS = 10'000'000;
constexpr int64_t GUARD_NS = 5'000'000;
constexpr int64_t POLL_NS = 5'000'000;
constexpr int64_t LOOKAHEAD_NS = 250'000'000;
constexpr int64_t REFILLS_PER_LOOKAHEAD = 16;

// returns false if stop was requested before the deadline
bool wait_until(clock_t_::time_point t, const std::atomic_bool& stop) {
	for (;;) {
		if (stop.load(std::memory_order_relaxed)) return false;
		const auto remaining = t - clock_t_::now();
		if (remaining <= 0ns) return true;
		if (remaining > 3ms) {
			std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(remaining - 2ms, 5ms));
		}
		else {
			CPU_RELAX();   // last ~3ms: spin
		}
	}
}

}

namespace andromeda::audio {

MaestroEngine::~MaestroEngine() {
	destroy();
}

void MaestroEngine::initialize() {
	if (_has_maestro) return;

	Library lib = open_library();
	if (!lib) {
		Debugger::log_error(std::format("Maestro ({}) is not installed.", LIBRARY_NAME));
		return;
	}

	MaestroGetInterfaceFn get_interface;
	*(void**)&get_interface = find_symbol(lib, "maestro_get_interface");
	if (!get_interface) return;

	core = (MaestroCoreV1*)get_interface(MAESTRO_CORE_V1);
	rt = (MaestroRealtimeV1*)get_interface(MAESTRO_REALTIME_V1);

	if (!core || !rt) {
		core = nullptr; rt = nullptr;
		Debugger::log_error("Maestro is unavailable.");
		return;
	}

	app = { sizeof app };
	app.app_id = "andromeda.audio.maestro";
	app.app_name = "Andromeda";
	
	MaestroResult r = rt->open(&app, NULL, &inst);
	if (r != MAESTRO_OK) {
		inst = nullptr;
		if (r == MAESTRO_ERR_DISABLED)
			Debugger::log_warning(std::format("Maestro has been disabled for this app. ({})", core->last_error()));
		else
			Debugger::log_error(std::format("Error while opening Maestro: {}", core->last_error()));
		return;
	}

	MaestroRealtimeInfo info{};
	info.struct_size = sizeof(info);
	if (rt->get_info(inst, &info) != MAESTRO_OK || info.max_schedule_us == 0) {
		Debugger::log_error(std::format("Error while getting Maestro info: {}", core->last_error()));
		rt->close(inst);
		return;
	}

	_max_schedule_time_us = info.max_schedule_us;
	_has_maestro = true;
	Debugger::log("Maestro initialized");
}

void MaestroEngine::destroy() {
	if (!_has_maestro) return;

	stop();

	if (inst) rt->close(inst);   // stop() above already did the full reset

	inst = nullptr;
	rt = nullptr;
	core = nullptr;
	_has_maestro = false;
}

void MaestroEngine::start() {
	if (!_has_maestro || _max_schedule_time_us == 0) return;

	stop();
	_stop_flag.store(false);

	auto* timer = _app->get_timer();
	const auto* timeline = _app->main_window()->editor_controller.get_project_manager()->get_timeline();
	// generation first: a seek landing between the two reads is then noticed (and handled) by the
	// player instead of lost
	const std::uint64_t seek_gen = timer->seek_generation();
	const double start_secs = timer->elapsed();
	const auto start_tick = static_cast<std::int64_t>(timeline->secs_to_tick(start_secs));

	_thread_working = true;
	_producer.start(start_tick, start_secs);
	_audio_thread = std::thread([=, this] { player_main(start_secs, seek_gen); });
}

void MaestroEngine::stop() {
	{
		// under the mutex so idle_wait() can't check the predicate, miss the flag and sleep through the notify
		std::lock_guard<std::mutex> lock(_schedule_mutex);
		_stop_flag.store(true);
	}
	_schedule_cv.notify_all();

	if (_audio_thread.joinable()) _audio_thread.join();
	_producer.stop();
	if (_has_maestro) rt->reset(inst);

	_play_at_mouse = false;
	_thread_working.store(false);
}

void MaestroEngine::reset() {
	// stop() already does the full reset; no is-playing early-out, since a stopped engine can
	// still hold state (e.g. from a mouse preview note)
	stop();
}

void MaestroEngine::start_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) {
	// update/stop_play_at_mouse refuse to run while playing, so a note started now could never be released
	if (!_has_maestro || is_playing()) return;

	const MaestroTime now = rt->now();
	if (_play_at_mouse)   // don't leave the previous preview note hanging
		rt->send_short(inst, 0, pack_message(0x80, _mouse_last_channel, _mouse_last_key, 0x00), now);
	rt->send_short(inst, 0, pack_message(0x90, channel, key, velocity), now);

	_play_at_mouse = true;
	_mouse_last_key = key;
	_mouse_last_channel = channel;
}

void MaestroEngine::update_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) {
	if (!_has_maestro || !_play_at_mouse || is_playing()) return;
	if (_mouse_last_key == key && _mouse_last_channel == channel) return;

	const MaestroTime now = rt->now();
	rt->send_short(inst, 0, pack_message(0x80, _mouse_last_channel, _mouse_last_key, 0x00), now);
	rt->send_short(inst, 0, pack_message(0x90, channel, key, velocity), now);

	_mouse_last_key = key;
	_mouse_last_channel = channel;
}

void MaestroEngine::stop_play_at_mouse(std::uint8_t key, std::uint8_t channel) {
	if (!_has_maestro || !_play_at_mouse || is_playing()) return;
	(void)key; (void)channel;
	rt->send_short(inst, 0, pack_message(0x80, _mouse_last_channel, _mouse_last_key, 0x00), rt->now());
	_play_at_mouse = false;
}

void MaestroEngine::player_main(double start_secs, std::uint64_t seen_gen) {
#if defined(_WIN32)
	timeBeginPeriod(1);
	DWORD task_idx = 0;
	HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
#endif

	const int64_t max_ns = static_cast<int64_t>(std::min<uint64_t>(_max_schedule_time_us, NS_LIMIT / 1000)) * 1000;
	const int64_t latency_ns = std::min(LATENCY_NS, max_ns / 20);
	const int64_t guard_ns = std::min(GUARD_NS, max_ns / 20);

	const int64_t ahead_ns = std::min(max_ns - guard_ns - latency_ns, LOOKAHEAD_NS);
	const int64_t refill_ns = std::max<int64_t>(ahead_ns / REFILLS_PER_LOOKAHEAD, POLL_NS);

	auto* timer = _app->get_timer();
	auto* project_manager = _app->main_window()->editor_controller.get_project_manager();
	const auto* timeline = project_manager->get_timeline();
	auto& ring = _producer.ring();
	auto& project_data = project_manager->get_project_data();
	const auto& tracks = project_data.tracks;

	// set when the timer stops under us; the next running iteration restarts the producer from
	// wherever the timer is
	bool paused = false;
	int64_t next_play_ns = 0;

	int64_t ref_ns = 0;
	MaestroTime ref_rt = 0;

	auto to_ns = [&](double secs) { return static_cast<int64_t>(secs * 1.0e9); };
	auto idle_wait = [&](int64_t ns) {
		std::unique_lock<std::mutex> lock(_schedule_mutex);
		_schedule_cv.wait_for(lock, std::chrono::nanoseconds(ns), [this] {
			return _stop_flag.load();
		});
	};
	auto anchor = [&] {
		ref_ns = to_ns(timer->elapsed() - start_secs);
		ref_rt = rt->now();
		next_play_ns = 0;
	};

	// track mute/solo
	using KeyBits = std::bitset<16 * 128>;
	std::vector<std::uint8_t> muted;
	std::vector<KeyBits> sounding;
	std::uint64_t mute_rev = 0;
	auto key_bit = [](std::uint32_t msg) { return ((msg & 0x0Fu) << 7) | ((msg >> 8) & 0x7Fu); };
	auto refresh_mutes = [&] {
		mute_rev = project_data.mute_revision.load(std::memory_order_acquire);
		muted.resize(tracks.size(), 0);
		sounding.resize(tracks.size());
		const MaestroTime now = rt->now();
		for (std::size_t i = 0; i < tracks.size(); ++i) {
			const bool m = tracks[i].muted;
			if (m && !muted[i] && sounding[i].any()) {
				// newly muted: release what it has sounding right now
				for (std::size_t b = 0; b < sounding[i].size(); ++b) {
					if (!sounding[i].test(b)) continue;
					rt->send_short(inst, 0, pack_message(0x80, static_cast<std::uint8_t>(b >> 7),
						static_cast<std::uint8_t>(b & 0x7F), 0x00), now);
				}
				sounding[i].reset();
			}
			muted[i] = m;
		}
	};

	// regenerate everything from `secs`. Only this thread consumes the ring, so the producer may
	// clear it. Callers drop Maestro's queue first.
	auto restart_at = [&](double secs) {
		start_secs = secs;
		for (auto& bits : sounding) bits.reset();   // Maestro was reset before this: nothing is sounding
		refresh_mutes();
		_producer.start(static_cast<std::int64_t>(timeline->secs_to_tick(secs)), secs);
		anchor();
	};

	// let producer generate a few events
	const auto t0 = clock_t_::now() + 50ms;
	if (wait_until(t0, _stop_flag)) {
		// get_start_time(), not start_secs: a seek during the wait must not be overwritten (the
		// generation check below then regenerates for it)
		// timer->start(timer->get_start_time());
		anchor();
	}
	refresh_mutes();

	while (!_stop_flag.load()) {
		// paused / stopped timer
		if (!timer->is_running()) {
			if (!paused) {
				rt->reset(inst);
				paused = true;
			}
			idle_wait(POLL_NS);
			continue;
		}

		// A seek (Playhead::set_start -> AudioTimer::navigate_to) while playing: what the producer
		// generated and what Maestro has queued belongs to the old position.
		if (const std::uint64_t g = timer->seek_generation(); g != seen_gen) {
			seen_gen = g;
			paused = false;
			rt->reset(inst);
			restart_at(timer->get_start_time());
		} else if (paused) {
			paused = false;
			restart_at(timer->elapsed());
		}

		if (project_data.mute_revision.load(std::memory_order_acquire) != mute_rev) refresh_mutes();

		const int64_t now_ns = to_ns(timer->elapsed() - start_secs);
		const auto wall_now = clock_t_::now();

		bool behind = false;
		if (now_ns >= next_play_ns) {
			const int64_t end_ns = now_ns + ahead_ns;

			loop {
				auto* ev = ring.peek();
				if (!ev) {
					behind = !_producer.done();
					break;
				}
				if (ev->time_ns >= end_ns) break;   // caught up to our lookahead

				if (ev->priority != 1 && ev->track < muted.size()) {
					auto& bits = sounding[ev->track];
					const std::uint32_t k = key_bit(ev->message);
					if (ev->priority == 2) {
						if (muted[ev->track]) { ring.pop(); continue; }
						bits.set(k);
					} else {
						if (!bits.test(k)) { ring.pop(); continue; }
						bits.reset(k);
					}
				}

				// events already due (offset < 0) all go out at the earliest slot; latency_ns keeps
				// that slot in the future so they aren't late, and stays inside max_schedule
				const int64_t offset_ns = std::max<int64_t>(0, ev->time_ns - ref_ns);
				const MaestroTime at = ref_rt + rt->ns_to_ticks(static_cast<uint64_t>(latency_ns + offset_ns));
				rt->send_short(inst, 0, ev->message, at);
				ring.pop();
			}

			next_play_ns = behind ? now_ns : now_ns + refill_ns;
		}

		// and we wait
		const int64_t timeline_now = now_ns +
			std::chrono::duration_cast<std::chrono::nanoseconds>(clock_t_::now() - wall_now).count();
		const int64_t remaining = next_play_ns - timeline_now;
		if (remaining > 0)
			idle_wait(std::min(remaining, POLL_NS));
		else if (behind)
			idle_wait(1'000'000);
	}

	rt->notes_off(inst);
	_thread_working.store(false);

#if defined(_WIN32)
	if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
	timeEndPeriod(1);
#endif
}

void MaestroEngine::render_settings() {
	if (!_has_maestro) return;
	if (ImGui::Button("Open Maestro settings"))
		rt->show_settings(inst);
}

}