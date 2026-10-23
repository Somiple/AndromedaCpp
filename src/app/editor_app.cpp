#include "editor_app.h"
#include "main_window.h"
#include "audio/midi_audio.h"
#include "audio/audio_timer.h"
#include "managers.h"

namespace andromeda::app {

namespace {
using managers::PlaybackManager;
}

EditorApp::EditorApp(MainWindow* main_window) : _main_window(main_window) {
	initialize();
}

void EditorApp::initialize() {
	_audio_engine = std::make_unique<audio::MIDIAudio>(this);
	_audio_timer = std::make_unique<audio::AudioTimer>();

	// once all resources initialize, initialize managers
	init_managers();

	_app_ready.store(true);
}

void EditorApp::init_managers() {
	_playback_manager = std::make_unique<PlaybackManager>(get_audio_engine(), get_timer());
}

}