#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>

namespace andromeda::audio {

// Engines start/stop/adjust this from their own threads while the UI reads elapsed(), so every
// field is atomic. Units are seconds throughout.
class AudioTimer {
public:
	AudioTimer() = default;

	void start(double start_time = 0.0);
	void stop();
	void navigate_to(double time_secs);

	// shifts elapsed() by `secs` (positive = later) without moving the seek point
	// (get_start_time) or raising the navigated flag. used to follow the audio clock.
	void adjust(double secs);

	[[nodiscard]] bool has_navigated_recently() { return has_navigated_.exchange(false); }
	[[nodiscard]] bool is_running() const { return running_.load(); }
	[[nodiscard]] double elapsed() const;
	[[nodiscard]] double get_start_time() const { return base_time_.load(); }

	void clear_flags() { has_navigated_ = false; }

private:
	using clock = std::chrono::steady_clock;

	static std::int64_t now_ns() {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
	}

	std::atomic<std::int64_t> start_ns_{ now_ns() };
	std::atomic<double> base_time_{ 0.0 };
	std::atomic<bool> running_{ false };
	// no pausing
	std::atomic<bool> has_navigated_{ false };
};

}