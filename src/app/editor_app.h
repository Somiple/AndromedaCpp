#pragma once

#include <memory>
#include <atomic>

namespace andromeda::audio {
class MIDIAudio;
class AudioTimer;
}

namespace andromeda::app {

namespace managers {
class PlaybackManager;
}

class MainWindow;

// macro!
#define MAKE_GETTER(type, name, value) \
	[[nodiscard]] type name() { return value; } \
	[[nodiscard]] const type name() const { return value; }

class EditorApp {
public:
	EditorApp(MainWindow* main_window);
	~EditorApp() = default;

	EditorApp(const EditorApp&) = delete;
	EditorApp& operator=(const EditorApp&) = delete;

#pragma region App setup
	void initialize();
	void init_managers();
#pragma endregion

	MAKE_GETTER(audio::AudioTimer*, get_timer, _audio_timer.get());

	MAKE_GETTER(managers::PlaybackManager*, get_playback_manager, _playback_manager.get());

	MAKE_GETTER(audio::MIDIAudio*, get_audio_engine, _audio_engine.get());

	// added here for compatibility, will remove when needed
	MAKE_GETTER(MainWindow*, main_window, _main_window);

	[[nodiscard]] bool ready() const { return _app_ready.load(); }

	// TODO: helper function for getting playhead's start tick/seconds
private:

#pragma region Main window
	MainWindow* _main_window;
#pragma endregion

#pragma region Audio
	std::unique_ptr<audio::AudioTimer> _audio_timer;
	// no separate playhead class this time, the audio timer's the playhead.
	std::unique_ptr<audio::MIDIAudio> _audio_engine;
#pragma endregion

#pragma region Managers
	std::unique_ptr<managers::PlaybackManager> _playback_manager;
#pragma endregion

#pragma region App state
	// synced
	std::atomic<bool> _app_ready{ false };
#pragma endregion
};

}