#include "midi_audio.h"
#include "realtime_engine.h"
#include "maestro_engine.h"
#include "editor/playhead.h"

namespace andromeda::audio {

MIDIAudio::MIDIAudio(app::MainWindow* app) {
	midi_out = std::make_unique<MIDIOut>();
	audio_engines[AUDIO_ENGINE_INDEX(Realtime)] = std::make_unique<RealtimeEngine>(app, midi_out.get());
	audio_engines[AUDIO_ENGINE_INDEX(Maestro)] = std::make_unique<MaestroEngine>(app);
}

void MIDIAudio::switch_engine(AudioEngineType engine) {
	if (engine == current_engine || engine == AudioEngineType::Count) return;

	AudioEngine* old_engine = get_current_engine();
	bool last_playing = old_engine->is_playing();
	old_engine->destroy();
	current_engine = engine;

	AudioEngine* new_engine = get_current_engine();
	new_engine->initialize();

	if (last_playing) start();
}

}