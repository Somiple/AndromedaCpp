#include "realtime_engine.h"

#include "util/util.h"
#include <algorithm>
#include <array>
#include <bitset>
#include <chrono>

#include "app/main_window.h"
#include "app/editor_app.h"
#include "app/managers.h"

#if defined(_WIN32)
#include <windows.h>
#include <timeapi.h>
#pragma comment(lib, "winmm.lib")
#include <avrt.h>
#pragma comment(lib, "avrt.lib")
#endif
#if defined(_M_X64) || defined(__x86_64__)
#include <immintrin.h>
#define CPU_RELAX() _mm_pause()
#else
#define CPU_RELAX() ((void)0)
#endif

namespace andromeda::audio {

using clock_t_ = std::chrono::steady_clock;
using namespace std::chrono_literals;

namespace {

// returns false if stop was requested before the deadline
bool wait_until(clock_t_::time_point t, const std::atomic_bool& stop) {
	for (;;) {
		if (stop.load(std::memory_order_relaxed)) return false;
		const auto remaining = t - clock_t_::now();
		if (remaining <= 0ns) return true;
		if (remaining > 3ms) {
			std::this_thread::sleep_for(std::min<std::chrono::nanoseconds>(remaining - 2ms, 5ms));
		} else {
			CPU_RELAX();   // last ~3ms: spin
		}
	}
}

}

void RealtimeEngine::start() {
	if (!_app) return;

	stop();
	stop_flag = false;

	const auto* timeline = _app->main_window()->editor_controller.get_project_manager()->get_timeline();
	const double start_secs = _app->get_timer()->elapsed();
	const auto start_tick = static_cast<std::int64_t>(timeline->secs_to_tick(start_secs));

	thread_working = true;
	_producer.start(start_tick, start_secs);
	audio_thread = std::thread([=, this] { player_main(start_secs); });
}

void RealtimeEngine::stop() {
	stop_flag = true;
	if (audio_thread.joinable()) audio_thread.join();
	_producer.stop();
	thread_working = false;
}

void RealtimeEngine::start_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) {
	if (play_at_mouse_ || is_playing()) {
		return;
	}

	_midi_out->send_event(pack_midi(0x90 | channel, key, velocity));
	play_at_mouse_ = true;
	mouse_last_key_ = key;
}

void RealtimeEngine::update_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) {
	if (!play_at_mouse_ || is_playing()) {
		return;
	}

	if (mouse_last_key_ == key) {
		return;
	}

	_midi_out->send_event(pack_midi(0x80 | channel, mouse_last_key_, 0x00));
	_midi_out->send_event(pack_midi(0x90 | channel, key, velocity));

	mouse_last_key_ = key;
}

void RealtimeEngine::stop_play_at_mouse(std::uint8_t key, std::uint8_t channel) {
	if (!play_at_mouse_ || is_playing()) {
		return;
	}
	// released the key under the mouse, not the one sounding (stuck note), so use the last one
	(void)key;

	_midi_out->send_event(pack_midi(0x80 | channel, mouse_last_key_, 0x00));
	play_at_mouse_ = false;
}

void RealtimeEngine::player_main(double start_secs) {
#if defined(_WIN32)
	timeBeginPeriod(1);
	DWORD task_idx = 0;
	HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &task_idx);
#endif

	auto* timer = _app->get_timer();
	auto& ring = _producer.ring();
	auto* project_manager = _app->main_window()->editor_controller.get_project_manager();
	auto& project_data = project_manager->get_project_data();
	const auto& tracks = project_data.tracks;

	// notes currently sounding per channel, so stop can release exactly those
	std::array<std::bitset<128>, 16> sounding{};
	auto send = [&](std::uint32_t m) {
		_midi_out->send_event(m);
		const std::uint8_t st = m & 0xF0, ch = m & 0x0F;
		const std::uint8_t key = (m >> 8) & 0x7F, vel = (m >> 16) & 0x7F;
		if (st == 0x90 && vel) sounding[ch].set(key);
		else if (st == 0x80 || st == 0x90) sounding[ch].reset(key);
	};

	// track mute/solo
	std::vector<bool> muted;
	std::uint8_t mute_rev = 0;
	auto refresh_mutes = [&] {
		mute_rev = project_data.mute_revision.load(std::memory_order_acquire);
		muted.resize(tracks.size(), 0);
		for (std::size_t i = 0; i < tracks.size(); i++) {
			const bool m = tracks[i].muted;
			if (m && !muted[i]) {
				// newly muted!
				for (std::size_t ch = 0; ch < 16; ch++) {
					for (std::size_t key = 0; key < 128; key++) {
						if (!sounding[ch].test(key)) continue;
						_midi_out->send_event(pack_midi(0x80 | ch, key, 0));
					}
				}
			}
			muted[i] = m;
		}
	};

	// small lead-in so the producer can fill the ring before the first deadline
	const auto t0 = clock_t_::now() + 50ms;
	if (wait_until(t0, stop_flag)) {
		// timer->start(start_secs);   // seconds, same origin as the event times
	}

	loop {
		if (stop_flag) break;

		if (project_data.mute_revision.load(std::memory_order_acquire) != mute_rev) refresh_mutes();

		ScheduledEvent* ev = ring.peek();
		// the producer may push its last event right before setting done
		if (!ev && _producer.done()) ev = ring.peek();
		if (!ev) {
			if (_producer.done()) break;
			CPU_RELAX();
			continue;
		}

		const auto deadline = t0 + std::chrono::nanoseconds(ev->time_ns);
		if (!wait_until(deadline, stop_flag)) break;

		// flush everything due now, so chords go out back-to-back
		const auto now = clock_t_::now();
		do {
			if (!muted[ev->track]) send(ev->message);
			ring.pop();
			ev = ring.peek();
		} while (ev && t0 + std::chrono::nanoseconds(ev->time_ns) <= now);
	}

	// release exactly what is sounding, then the blanket messages as a backstop
	for (std::uint8_t ch = 0; ch < 16; ++ch) {
		for (int key = 0; key < 128; ++key) {
			if (!sounding[ch].test(key)) continue;
			_midi_out->send_event(pack_midi(0x80 | ch, key, 0));
		}
		_midi_out->send_event(pack_midi(0xB0 | ch, 64, 0));    // sustain off
		_midi_out->send_event(pack_midi(0xB0 | ch, 123, 0));   // all notes off
		_midi_out->send_event(pack_midi(0xB0 | ch, 120, 0));   // all sound off
	}

	// timer->stop();
	thread_working = false;

#if defined(_WIN32)
	if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
	timeEndPeriod(1);
#endif
}

void RealtimeEngine::render_settings() {
	ImGui::Text("Open OmniMIDI to adjust settings");
}

}