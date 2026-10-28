#pragma once

#include <memory>
#include <atomic>

#include "core/util/macros.h"

namespace andromeda::audio {
class MIDIAudio;
class AudioTimer;
}

namespace andromeda::editor {
class IPlaybackProvider;
}

namespace andromeda::app {

namespace managers {
class PlaybackManager;
}

class MainWindow;

class EditorApp {
public:
	EditorApp(MainWindow* main_window);
	// REFACTOR: out-of-line because the owned provider is an incomplete type in this header,
	// so the unique_ptr destructor must be instantiated in the .cpp instead.
	~EditorApp();

	EditorApp(const EditorApp&) = delete;
	EditorApp& operator=(const EditorApp&) = delete;

#pragma region App setup
	void initialize();
	void init_managers();
#pragma endregion

	MAKE_GETTER(audio::AudioTimer*, get_timer, _audio_timer.get());

	MAKE_GETTER(managers::PlaybackManager*, get_playback_manager, _playback_manager.get());

	// REFACTOR: narrow transport view handed to editor components; the adapter is the only
	// place that knows managers::PlaybackManager, keeping the editor layer ui-free.
	MAKE_GETTER(editor::IPlaybackProvider*, get_playback_provider, _playback_provider.get());

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
	// REFACTOR: owns the adapter so the IPlaybackProvider handed to editor components
	// outlives every EditorContext that references it. Stored as the interface pointer so
	// the inline getter needs no conversion and this header stays free of the adapter.
	std::unique_ptr<editor::IPlaybackProvider> _playback_provider;
#pragma endregion

#pragma region App state
	// synced
	std::atomic<bool> _app_ready{ false };
#pragma endregion
};

}