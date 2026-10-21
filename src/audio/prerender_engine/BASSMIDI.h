#pragma once

#include <mutex>
#include <bass.h>
#include <bassmidi.h>
#include <vector>

namespace andromeda::audio {

class BASSMIDI {
public:
	HSTREAM stream_handle;

	bool can_seek = false;
	long long position = 0;
	long long length = 0;

	static void init_bass(WAVEFORMATEX format);
	static void dispose_bass() { BASS_Free(); }

	BASSMIDI(int voices, bool no_fx);
	~BASSMIDI() { BASS_StreamFree(stream_handle); }

	static void free_soundfonts();
	static void load_soundfonts(const std::vector<std::wstring>& paths);

	bool write_bass(int buf_len, unsigned long* progress);
	float* write_float_array(int buf_len, unsigned long* progress);
	int k_short_message(int dw_param1, int sample_offset);

	DWORD send_event(DWORD type, DWORD param, DWORD chan, DWORD tick, DWORD time) {
		BASS_MIDI_EVENT ev = {
			type = type,
			param = param,
			chan = chan,
			tick = tick,
			time = time << 3
		};
		DWORD mode = BASS_MIDI_EVENTS_TIME | BASS_MIDI_EVENTS_STRUCT;
		return BASS_MIDI_StreamEvents(stream_handle, mode, &ev, 1);
	}

	DWORD send_event_raw(BYTE data[], DWORD count) {
		return BASS_MIDI_StreamEvents(stream_handle, BASS_MIDI_EVENTS_RAW | BASS_MIDI_EVENTS_NORSTATUS, data, count);
	}

	DWORD bass_stream_events(BASS_MIDI_EVENT events[], int count) {
		DWORD mode = BASS_MIDI_EVENTS_TIME | BASS_MIDI_EVENTS_STRUCT;
		return BASS_MIDI_StreamEvents(stream_handle, mode, events, count);
	}

	DWORD read(float* buffer, int offset, int count);
};

}