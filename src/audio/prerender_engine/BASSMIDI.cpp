#include "BASSMIDI.h"
#include <algorithm>
#include <vector>
#include <sstream>
#include <iostream>
#include "util/debugger.h"

using andromeda::util::Debugger;

namespace andromeda::audio {

static WAVEFORMATEX wave_format_static = WAVEFORMATEX{};
static int font_count = 0;
static BASS_MIDI_FONTEX* font_array = (BASS_MIDI_FONTEX*)malloc(sizeof(BASS_MIDI_FONTEX));
static std::mutex sf_lock;
static std::mutex mutex;

void BASSMIDI::init_bass(WAVEFORMATEX format) {
	wave_format_static = format;
	BASS_Free();

	if (!BASS_Init(-1, wave_format_static.nSamplesPerSec, 0, NULL, NULL)) {
		int err = BASS_ErrorGetCode();
		Debugger::log_error(std::format("BASS_Init failed! Error code: {}", err));
	}
}

BASSMIDI::BASSMIDI(int voices, bool no_fx = false) {
	stream_handle = BASS_MIDI_StreamCreate(16,
		BASS_SAMPLE_FLOAT |
		BASS_STREAM_DECODE |
		BASS_MIDI_SINCINTER |
		// BASS_MIDI_NOTEOFF1 |
		0x800000,
		wave_format_static.nSamplesPerSec);

	if (stream_handle == -1) {
#ifdef _WIN32
		int err = BASS_ErrorGetCode();
		MessageBoxW(NULL, L"BASSMIDI Handle failed to load!\0", L"Error\0", MB_ICONERROR);
#endif
	}

	BASS_ChannelSetAttribute(stream_handle, BASS_ATTRIB_MIDI_VOICES, voices);
	BASS_ChannelSetAttribute(stream_handle, BASS_ATTRIB_SRC, 3);
	BASS_ChannelSetAttribute(stream_handle, BASS_ATTRIB_MIDI_CHANS, 16);

	if (no_fx) BASS_ChannelFlags(stream_handle, BASS_MIDI_NOFX, BASS_MIDI_NOFX);

	{
		std::lock_guard<std::mutex> lock(sf_lock);
		BASS_MIDI_StreamSetFonts(stream_handle, font_array, 1);
	}
}

void BASSMIDI::free_soundfonts() {
	if (font_array == NULL) return;

	for (int i = 0; i < font_count; i++) {
		BASS_MIDI_FontFree((*font_array).font);
	}
	free(font_array);
	font_array = NULL;
	font_count = 0;
}

void BASSMIDI::load_soundfonts(const std::vector<std::wstring>& paths)
{
	std::lock_guard<std::mutex> lock(sf_lock);

	free_soundfonts();

	if (paths.empty())
	{
		font_array = nullptr;
		font_count = 0;
		lock.~lock_guard();
		return;
	}

	font_count = (int)paths.size();
	BASS_MIDI_FONTEX* fonts = (BASS_MIDI_FONTEX*)malloc(font_count * sizeof(BASS_MIDI_FONTEX));

	for (int i = 0; i < font_count; i++)
	{
		HSOUNDFONT font = BASS_MIDI_FontInit((void*)paths[i].c_str(), BASS_UNICODE);
		if (font != 0)
		{
			fonts[0].font = font;
			fonts[0].spreset = -1;
			fonts[0].sbank = -1;
			fonts[0].dpreset = -1;
			fonts[0].dbank = 0;
			fonts[0].dbanklsb = 0;

			BASS_MIDI_FontLoad(font, -1, -1);
		}
		else
		{
			std::stringstream err;
			err << "Soudfont failed to load! Err Code: " << BASS_ErrorGetCode() << "\0";
			// std::cout << err.str().c_str() << std::endl;
			Debugger::log_error(err.str());
			fonts[i].font = NULL;
		}
	}

	font_array = fonts;
}

bool BASSMIDI::write_bass(int buf_len, unsigned long* progress)
{
	buf_len <<= 3;
	unsigned char* buf;

	DWORD ret = BASS_ChannelGetData(stream_handle, &buf, buf_len);
	if (ret > 0)
	{
		(*progress) += (unsigned int)ret;
		return true;
	}
	else
	{
		int err = BASS_ErrorGetCode();
		return false;
	}
}

float* BASSMIDI::write_float_array(int buf_len, unsigned long* progress)
{
	unsigned char* buf = (unsigned char*)malloc(buf_len * 4 * sizeof(unsigned char));
	float* flt = (float*)malloc(buf_len * sizeof(float));

	DWORD ret = BASS_ChannelGetData(stream_handle, &buf, buf_len * 4);
	if (ret > 0) {
		(*progress) += (unsigned int)ret;
		memcpy(flt, buf, sizeof(buf));
		return flt;
	}
	else
	{
		int err = BASS_ErrorGetCode();
		return nullptr;
	}
}

int BASSMIDI::k_short_message(int dw_param1, int sample_offset)
{
	if ((unsigned char)dw_param1 == 0xFF) return 1;

	unsigned char cmd = (unsigned char)dw_param1;

	BASS_MIDI_EVENT ev;

	if (cmd < 0xA0)
	{

		ev.event = MIDI_EVENT_NOTE;
		ev.param = cmd < 0x90 ? (unsigned char)(dw_param1 >> 8) : (unsigned short)(dw_param1 >> 8);
		ev.chan = (int)dw_param1 & 0xF;
		ev.tick = 0;
		ev.pos = sample_offset << 3;
	}
	else if (cmd < 0xB0)
	{
		ev.event = MIDI_EVENT_KEYPRES;
		ev.param = (unsigned short)dw_param1 >> 8;
		ev.chan = (int)dw_param1 & 0xF;
		ev.tick = 0;
		ev.pos = sample_offset << 3;
	}
	else if (cmd < 0xC0)
	{
		// TODO
		return 0;
	}
	else if (cmd < 0xD0)
	{
		ev.event = MIDI_EVENT_PROGRAM;
		ev.param = (unsigned char)(dw_param1 >> 8);
		ev.chan = (int)dw_param1 & 0xF;
		ev.tick = 0;
		ev.pos = sample_offset << 3;
	}
	else if (cmd < 0xE0)
	{
		ev.event = MIDI_EVENT_CHANPRES;
		ev.param = (unsigned char)(dw_param1 >> 8);
		ev.chan = (int)dw_param1 & 0xF;
		ev.tick = 0;
		ev.pos = sample_offset << 3;
	}
	else if (cmd == 0xF0)
	{
		ev.event = MIDI_EVENT_PITCH;
		ev.param = (int)((unsigned char)(dw_param1 >> 16) | ((dw_param1 & 0x7F00) >> 1));
		ev.chan = (int)dw_param1 & 0xF;
		ev.tick = 0;
		ev.pos = sample_offset << 3;
	}
	else return 0;

	BASS_MIDI_EVENT evs[1] = { ev };

	bass_stream_events(evs, 1);

	return 0;
}

DWORD BASSMIDI::read(float* buffer, int offset, int count) {
	DWORD size = count * sizeof(float);
	DWORD ret = BASS_ChannelGetData(stream_handle, buffer + offset, size | BASS_DATA_FLOAT);

	if (ret == (DWORD)-1)
	{
#ifdef _WIN32
		int err = BASS_ErrorGetCode();
		MessageBoxW(NULL, L"Error\0", L"Error\0", MB_ICONERROR);
#endif
		return 0;
	}
	return ret / 4;
}

}