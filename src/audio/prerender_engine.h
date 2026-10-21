#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "event_producer.h"
#include "midi_audio.h"

namespace andromeda::audio {

// a stub for now, until we figure some stuff out
class PrerenderEngine : public AudioEngine
{
public:
	explicit PrerenderEngine(app::MainWindow* app) : AudioEngine(app) {}

	[[nodiscard]] bool is_supported() const override { return true; }
	[[nodiscard]] std::string get_name() const override { return "Prerendered Audio"; }

	void initialize() override;
	void destroy() override;

	void start() override;
	void stop() override;
	void reset() override;
	bool is_playing() override;

	void render_settings() override;
};

}