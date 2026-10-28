#pragma once

// ---------------------------------------------------------------------------
// Layer: audio
// Dependency direction:  core -> midi -> editor -> audio -> ui
// This layer may include `core/...`, `midi/...` and `editor/...` only; it must
// stay free of ImGui, GLFW and GL headers so playback can run without a window.
// See docs/ARCHITECTURE.md.
// ---------------------------------------------------------------------------

#include <array>
#include <memory>
#include "editor/project/project_manager.h"
#include "audio_timer.h"
#include "midi_out.h"

namespace andromeda::app {
class MainWindow;
class EditorApp;
}

namespace andromeda::audio {

using andromeda::editor::ProjectManager;

class AudioEngine {
	friend class MIDIAudio;

public:
	explicit AudioEngine(app::EditorApp* app)
		: _app(app) { }

	virtual ~AudioEngine() = default;
	virtual void initialize() = 0;
	virtual void destroy() = 0;

	[[nodiscard]] virtual bool is_supported() const { return true; }

	virtual void start() = 0;
	virtual void stop() = 0;
	virtual void reset() = 0;
	[[nodiscard]] virtual bool is_playing() = 0;
	
	// mouse stuff
	virtual void start_play_at_mouse(uint8_t key, uint8_t channel, uint8_t velocity) {}
	virtual void update_play_at_mouse(uint8_t key, uint8_t channel, uint8_t velocity) {}
	virtual void stop_play_at_mouse(uint8_t key, uint8_t channel) {}

	// displays the engine's settings which displays in the editor settings window
	[[nodiscard]] virtual std::string get_name() const { return "Audio engine"; }
	virtual void render_settings() {}
protected:
	app::EditorApp* _app;
};

enum class AudioEngineType {
	Realtime,
	Maestro,
	Count
};

#define AUDIO_ENGINE_INDEX(name) static_cast<size_t>(AudioEngineType::name)
#define AUDIO_ENGINE_INDEX_VAR(var) static_cast<size_t>(var)

typedef std::array<std::unique_ptr<AudioEngine>, AUDIO_ENGINE_INDEX(Count)> AudioEngineList;

class MIDIAudio
{
public:
	MIDIAudio(app::EditorApp* app);

	void start() {
		get_current_engine()->start();
	}

	void stop() {
		get_current_engine()->stop();
	}

	void reset() {
		get_current_engine()->reset();
	}

	[[nodiscard]] AudioEngine* get_current_engine() const {
		return audio_engines[AUDIO_ENGINE_INDEX_VAR(current_engine)].get();
	}

	[[nodiscard]] const AudioEngineType get_current_engine_type() const { return current_engine; }

	void switch_engine(AudioEngineType engine);
	[[nodiscard]] AudioEngineList& get_engine_list() { return audio_engines; }

private:
	std::unique_ptr<MIDIOut> midi_out;
	AudioEngineType current_engine = AudioEngineType::Realtime;

	AudioEngineList audio_engines{ nullptr };
	app::EditorApp* _app;
};

}