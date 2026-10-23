#pragma once

#include <atomic>
#include <cstdint>
#include <thread>

#include "event_producer.h"
#include "midi_audio.h"
#include "midi_out.h"

namespace andromeda::app {
class EditorApp;
}

namespace andromeda::audio {

class RealtimeEngine : public AudioEngine
{
public:
	explicit RealtimeEngine(app::EditorApp* app, MIDIOut* midi_out)
		: AudioEngine(app), _midi_out(midi_out), _producer(app) {}

	~RealtimeEngine() override { stop(); }

	[[nodiscard]] bool is_supported() const override { return true; }
	[[nodiscard]] std::string get_name() const override { return "Realtime (MIDI out)"; }

	void initialize() override {}
	void destroy() override { stop(); }

	void start() override;
	void stop() override;

	void reset() override {
		if (!thread_working) return;
		stop();
	}

	bool is_playing() override { return thread_working; }

	void start_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) override;
	void update_play_at_mouse(std::uint8_t key, std::uint8_t channel, std::uint8_t velocity) override;
	void stop_play_at_mouse(std::uint8_t key, std::uint8_t channel) override;

	void render_settings() override;
private:
	void player_main(double start_secs);

	MIDIOut* _midi_out;
	EventProducer _producer;

	std::atomic_bool stop_flag = false;
	std::atomic_bool thread_working = false;
	std::thread audio_thread;

	bool play_at_mouse_ = false;
	std::uint8_t mouse_last_key_ = 0;
};

}