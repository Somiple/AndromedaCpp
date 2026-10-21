#pragma once

#include <cstdint>
#include <vector>

#ifdef _WIN32
#include <Windows.h>
#endif

namespace andromeda::audio {

#ifdef _WIN32

typedef void (*KDMAPI_Init)();
typedef void (*KDMAPI_Terminate)();
typedef void (*KDMAPI_Send)(uint32_t msg);

class MIDIOut
{
public:
	MIDIOut();
	~MIDIOut();
	void send_event(uint32_t msg);
private:
	HMODULE omni = nullptr;

	KDMAPI_Init Init = nullptr;
	KDMAPI_Send Send = nullptr;
	KDMAPI_Terminate End = nullptr;

	bool initialized = false;
};

#else

class MIDIOut
{
public:
	MIDIOut();
	~MIDIOut();
	void send_event(uint32_t msg);
private:
	void* handle = nullptr;
	void (*sendDirectData)(uint32_t msg) = nullptr;
	void (*terminateStream)() = nullptr;
};
#endif

}