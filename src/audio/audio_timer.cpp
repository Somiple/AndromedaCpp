#include "audio_timer.h"

namespace andromeda::audio {

void AudioTimer::start(double start_time) {
	base_time_ = start_time;
	start_ns_ = now_ns();
	running_ = true;
	has_navigated_ = true;
}

void AudioTimer::stop() {
	running_ = false;
}

void AudioTimer::navigate_to(double time_secs) {
	base_time_ = time_secs;
	if (running_) start_ns_ = now_ns();
	has_navigated_ = true;
}

void AudioTimer::adjust(double secs) {
	// elapsed() = base + (now - start), so moving the start earlier moves elapsed() later
	start_ns_ -= static_cast<std::int64_t>(secs * 1e9);
}

double AudioTimer::elapsed() const {
	const double base = base_time_.load();
	if (!running_.load()) return base;
	return base + static_cast<double>(now_ns() - start_ns_.load()) * 1e-9;
}

}