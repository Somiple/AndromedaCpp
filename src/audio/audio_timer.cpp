#include "audio_timer.h"

namespace andromeda::audio {

void AudioTimer::start(double start_time) {
	std::lock_guard lock(mu_);
	base_time_ = start_time;
	start_ns_ = now_ns();
	running_ = true;
	has_navigated_ = true;
}

void AudioTimer::stop() {
	std::lock_guard lock(mu_);
	running_ = false;
}

void AudioTimer::navigate_to(double time_secs) {
	std::lock_guard lock(mu_);
	base_time_ = time_secs;
	if (running_) start_ns_ = now_ns();
	has_navigated_ = true;
	// last, so whoever sees the new generation also sees the new base_time_
	seek_gen_.fetch_add(1, std::memory_order_release);
}

void AudioTimer::adjust(double secs) {
	std::lock_guard lock(mu_);
	// elapsed() = base + (now - start), so moving the start earlier moves elapsed() later
	start_ns_ -= static_cast<std::int64_t>(secs * 1e9);
}

double AudioTimer::elapsed() const {
	std::lock_guard lock(mu_);
	if (!running_) return base_time_;
	return base_time_ + static_cast<double>(now_ns() - start_ns_) * 1e-9;
}

double AudioTimer::get_start_time() const {
	std::lock_guard lock(mu_);
	return base_time_;
}

}