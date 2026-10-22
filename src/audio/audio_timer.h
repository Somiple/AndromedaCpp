#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace andromeda::audio {

// Engines start/stop/adjust this from their own threads while the UI reads elapsed(). base_time_
// and start_ns_ have to change together (elapsed() is base + (now - start)), so they live under a
// mutex; the flags and the seek counter stay atomic. Units are seconds throughout.
class AudioTimer {
public:
	AudioTimer() = default;

	void start(double start_time = 0.0);
	void stop();

	// moves the seek point to time_secs (and the running clock with it). bumps seek_generation()
	void navigate_to(double time_secs);

	// shifts elapsed() by `secs` (positive = later) without moving the seek point
	// (get_start_time) or raising the navigated flag. used to follow the audio clock.
	void adjust(double secs);

	[[nodiscard]] bool has_navigated_recently() { return has_navigated_.exchange(false); }

	// Counts navigate_to() calls. Unlike has_navigated_recently() reading it consumes nothing, so
	// several observers (the UI, an audio engine) can each notice a seek on their own.
	[[nodiscard]] std::uint64_t seek_generation() const { return seek_gen_.load(std::memory_order_acquire); }

	[[nodiscard]] bool is_running() const { return running_.load(); }
	[[nodiscard]] double elapsed() const;
	[[nodiscard]] double get_start_time() const;

	void clear_flags() { has_navigated_ = false; }

private:
	using clock = std::chrono::steady_clock;

	static std::int64_t now_ns() {
		return std::chrono::duration_cast<std::chrono::nanoseconds>(clock::now().time_since_epoch()).count();
	}

	mutable std::mutex mu_;
	std::int64_t start_ns_ = now_ns();   // guarded by mu_
	double base_time_ = 0.0;             // guarded by mu_

	std::atomic<bool> running_{ false };   // written under mu_, readable anywhere
	// no pausing
	std::atomic<bool> has_navigated_{ false };
	std::atomic<std::uint64_t> seek_gen_{ 0 };
};

}