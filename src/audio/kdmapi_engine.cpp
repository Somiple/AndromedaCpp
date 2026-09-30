#include "audio/kdmapi_engine.h"

#include <format>

#ifdef _WIN32
#include <windows.h>
#endif

#include "util/debugger.h"

namespace andromeda::audio::kdmapi {

using util::Debugger;

struct KDMAPI::Lib {
#ifdef _WIN32
    HMODULE module = nullptr;
#endif
    int (*IsKDMAPIAvailable)() = nullptr;
    int (*InitializeKDMAPIStream)() = nullptr;
    int (*TerminateKDMAPIStream)() = nullptr;
    void (*SendDirectData)(std::uint32_t) = nullptr;

    [[nodiscard]] bool valid() const {
        return InitializeKDMAPIStream != nullptr && TerminateKDMAPIStream != nullptr &&
               SendDirectData != nullptr;
    }
};

KDMAPI::Lib* KDMAPI::load_lib() {
    static Lib lib = [] {
        Lib l;
#ifdef _WIN32
        l.module = LoadLibraryW(L"OmniMIDI.dll");
        if (l.module == nullptr) {
            return l;
        }

        l.IsKDMAPIAvailable = reinterpret_cast<int (*)()>(
            reinterpret_cast<void*>(GetProcAddress(l.module, "IsKDMAPIAvailable")));
        l.InitializeKDMAPIStream = reinterpret_cast<int (*)()>(
            reinterpret_cast<void*>(GetProcAddress(l.module, "InitializeKDMAPIStream")));
        l.TerminateKDMAPIStream = reinterpret_cast<int (*)()>(
            reinterpret_cast<void*>(GetProcAddress(l.module, "TerminateKDMAPIStream")));
        l.SendDirectData = reinterpret_cast<void (*)(std::uint32_t)>(
            reinterpret_cast<void*>(GetProcAddress(l.module, "SendDirectData")));
#endif
        return l;
    }();

    return &lib;
}

KDMAPI::KDMAPI() {
    const Lib* lib = load_lib();

    if (lib->valid()) {
        Debugger::log("KDMAPI loaded!");
    } else {
        Debugger::log_error(
            "KDMAPI needs to be installed in order to use it. Details: OmniMIDI.dll not found "
            "or missing exports");
    }
}

KDMAPI::~KDMAPI() {
    Debugger::log("Closing KDMAPI...");
    close();
}

void KDMAPI::init() {
    if (stream_open_) {
        return;
    }

    const Lib* lib = load_lib();

    if (!lib->valid()) {
        Debugger::log_error("KDMAPI not found or installed! Details: OmniMIDI.dll unavailable");
        return;
    }

    if (lib->IsKDMAPIAvailable != nullptr && lib->IsKDMAPIAvailable() == 0) {
        Debugger::log_error("Failed to start KDMAPI streaming! Details: KDMAPI reports unavailable");
        return;
    }

    if (lib->InitializeKDMAPIStream() == 0) {
        Debugger::log_error("Failed to start KDMAPI streaming! Details: InitializeKDMAPIStream failed");
        return;
    }

    stream_open_ = true;
}

void KDMAPI::close() {
    if (!stream_open_) {
        return;
    }

    const Lib* lib = load_lib();
    if (lib->valid()) {
        lib->TerminateKDMAPIStream();
    }
    stream_open_ = false;
}

void KDMAPI::init_audio() { init(); }

void KDMAPI::close_stream() { close(); }

std::expected<void, std::string> KDMAPI::send_event(std::span<const std::uint8_t> raw_event) {
    if (!stream_open_) {
        Debugger::log_warning("KDMAPI stream was never initialized. Initializing automatically...");
        init_audio();
    }

    if (stream_open_) {
        std::uint32_t ev = 0;
        for (std::size_t i = 0; i < raw_event.size() && i < 3; ++i) {
            ev |= static_cast<std::uint32_t>(raw_event[i]) << (8 * i);
        }
        load_lib()->SendDirectData(ev);
    } else {
        Debugger::log_error("KDMAPI Stream is not available.");
    }

    return {};
}

void KDMAPI::send_events(std::span<const std::array<std::uint8_t, 3>> events) {
    if (events.empty()) {
        return;
    }

    if (!stream_open_) {
        Debugger::log_warning("KDMAPI stream was never initialized. Initializing automatically...");
        init_audio();
    }
    if (!stream_open_) {
        Debugger::log_error("KDMAPI Stream is not available.");
        return;
    }

    const Lib* lib = load_lib();
    if (lib->SendDirectData == nullptr) {
        return;
    }

    for (const std::array<std::uint8_t, 3>& ev : events) {
        lib->SendDirectData(static_cast<std::uint32_t>(ev[0]) |
                            (static_cast<std::uint32_t>(ev[1]) << 8) |
                            (static_cast<std::uint32_t>(ev[2]) << 16));
    }
}

}
