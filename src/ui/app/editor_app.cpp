#include "editor_app.h"
#include "main_window.h"
#include "audio/midi_audio.h"
#include "audio/audio_timer.h"
#include "managers.h"
#include "playback_provider_adapter.h"

namespace {
using andromeda::config::EditorConfig;
}

namespace andromeda::app {

EditorApp::EditorApp(MainWindow* main_window) : _main_window(main_window) {
	initialize();
}

EditorApp::~EditorApp() {
	_config.save_config();
}

void EditorApp::initialize() {
	_config.load_config_or_default();
	_audio_engine = std::make_unique<audio::MIDIAudio>(this);
	_audio_engine->switch_engine(static_cast<audio::AudioEngineType>(_config.audio.audio_engine_id));

	_audio_timer = std::make_unique<audio::AudioTimer>();

	// once all resources initialize, initialize managers
	init_managers();

	_app_ready.store(true);
}

void EditorApp::init_managers() {
	// TODO: eventually have EditorApp be the owner of the editor controller
	//       so that we can easily get project manager from there instead
	auto* timeline = main_window()->controller().get_project_manager()->get_timeline();
	_playback_manager = std::make_unique<managers::PlaybackManager>(get_audio_engine(), get_timer(), timeline);

	// REFACTOR: the adapter is built right where the transport manager is built, so the
	// IPlaybackProvider handed to editor components never outlives its backing manager.
	_playback_provider = std::make_unique<PlaybackProviderAdapter>(*_playback_manager);
}

}