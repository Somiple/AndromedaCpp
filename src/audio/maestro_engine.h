#pragma once

#include "event_producer.h"
#include "midi_audio.h"
#include "midi_out.h"

#define MAESTRO_NO_PROTOTYPES
#include "maestro.h"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace andromeda::app {
class MainWindow;
}

namespace andromeda::audio {

class MaestroEngine : public AudioEngine {
public:
	MaestroEngine(app::MainWindow* app) : AudioEngine(app), _producer(app) {}
	~MaestroEngine() override;

	[[nodiscard]] bool is_supported() const override { return true; }
	[[nodiscard]] std::string get_name() const override { return "Maestro"; }

	void initialize() override;
	void destroy() override;

	void start() override;
	void stop() override;
	void reset() override;

	bool is_playing() override { return _thread_working.load(); }

	void start_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) override;
	void update_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) override;
	void stop_play_at_mouse(std::uint8_t key, std::uint8_t channel) override;

	void render_settings() override;
private:
	void player_main(double start_secs, std::uint64_t seek_gen);

	MaestroCoreV1* core = nullptr;
	MaestroRealtimeV1* rt = nullptr;
	MaestroRealtime* inst = nullptr;
	MaestroAppInfo app{};

	bool _has_maestro = false;
	std::uint64_t _max_schedule_time_us{ 0 };

	EventProducer _producer;

	std::atomic_bool _stop_flag = false;
	std::atomic_bool _thread_working = false;
	std::thread _audio_thread;

	std::condition_variable _schedule_cv;
	std::mutex _schedule_mutex;

	bool _play_at_mouse = false;
	std::uint8_t _mouse_last_key{ 0 };
	std::uint8_t _mouse_last_channel{ 0 };
};

}