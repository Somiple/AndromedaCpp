#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <thread>

namespace andromeda::app {
class MainWindow;
}

namespace andromeda::audio {

// status | data1 << 8 | data2 << 16
constexpr std::uint32_t pack_midi(std::uint32_t status, std::uint32_t d1, std::uint32_t d2) {
	return status | (d1 << 8) | (d2 << 16);
}

// one MIDI message due `time_ns` after playback start
struct ScheduledEvent {
	std::int64_t  time_ns = 0;
	std::uint32_t message = 0;
	std::uint8_t  priority = 0;  // 0 = note-off, 1 = channel event, 2 = note-on (orders events at the same time)
};

// single-producer single-consumer ring buffer
template <typename T, std::size_t N>
class SpscRing {
	static_assert(N && !(N & (N - 1)), "N must be a power of two");

public:
	bool push(const T& v) {
		const auto h = head_.load(std::memory_order_relaxed);
		if (h - tail_.load(std::memory_order_acquire) == N) return false;
		buf_[h & (N - 1)] = v;
		head_.store(h + 1, std::memory_order_release);
		return true;
	}

	// oldest unconsumed element, or nullptr when empty. valid until pop()
	T* peek() {
		const auto t = tail_.load(std::memory_order_relaxed);
		if (t == head_.load(std::memory_order_acquire)) return nullptr;
		return &buf_[t & (N - 1)];
	}

	void pop() { tail_.store(tail_.load(std::memory_order_relaxed) + 1, std::memory_order_release); }

	// only while neither the producer nor the consumer is running
	void clear() { tail_.store(head_.load(std::memory_order_acquire), std::memory_order_relaxed); }

private:
	std::array<T, N> buf_{};
	alignas(64) std::atomic<std::size_t> head_{ 0 };
	alignas(64) std::atomic<std::size_t> tail_{ 0 };
};

// Walks every track and pushes ScheduledEvents, in time order, into a ring while the engine
// consumes them. The same producer feeds the realtime and the prerender engine; only what the
// consumer does with an event differs.
//
// start() must only be called while the consumer is not reading the ring.
class EventProducer {
public:
	using Ring = SpscRing<ScheduledEvent, 1 << 16>;

	explicit EventProducer(app::MainWindow* app) : _app(app) {}
	~EventProducer() { stop(); }

	EventProducer(const EventProducer&) = delete;
	EventProducer& operator=(const EventProducer&) = delete;

	// event times are relative to start_secs; notes starting before start_tick are skipped, but
	// program/controller/bend events before it are sent at t=0 so the sound is right mid-song
	void start(std::int64_t start_tick, double start_secs);
	void stop();

	[[nodiscard]] Ring& ring() { return _ring; }

	// true once every event has been pushed (not necessarily consumed)
	[[nodiscard]] bool done() const { return _done.load(std::memory_order_acquire); }

private:
	void run(std::int64_t start_tick, double start_secs);

	app::MainWindow* _app;
	Ring _ring;
	std::thread _thread;
	std::atomic_bool _stop{ false };
	std::atomic_bool _done{ false };
};

}