#include "audio/soft_synth.h"

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <format>

#include "util/debugger.h"

#ifdef _WIN32
#include <windows.h>
#endif

namespace andromeda::audio {

using util::Debugger;

namespace {

#ifdef _WIN32

using fluid_settings_t = void;
using fluid_synth_t = void;

using new_fluid_settings_fn = fluid_settings_t* (*)();
using delete_fluid_settings_fn = void (*)(fluid_settings_t*);
using fluid_settings_setnum_fn = int (*)(fluid_settings_t*, const char*, double);
using fluid_settings_setint_fn = int (*)(fluid_settings_t*, const char*, int);
using new_fluid_synth_fn = fluid_synth_t* (*)(fluid_settings_t*);
using delete_fluid_synth_fn = void (*)(fluid_synth_t*);
using fluid_synth_sfload_fn = int (*)(fluid_synth_t*, const char*, int);
using fluid_synth_noteon_fn = int (*)(fluid_synth_t*, int, int, int);
using fluid_synth_noteoff_fn = int (*)(fluid_synth_t*, int, int);
using fluid_synth_cc_fn = int (*)(fluid_synth_t*, int, int, int);
using fluid_synth_pitch_bend_fn = int (*)(fluid_synth_t*, int, int);
using fluid_synth_all_notes_off_fn = int (*)(fluid_synth_t*, int);
using fluid_synth_system_reset_fn = int (*)(fluid_synth_t*);
using fluid_synth_write_float_fn = int (*)(fluid_synth_t*, int, void*, int, int, void*, int, int);

class FluidSynthBackend final : public SoftSynth {
public:
    ~FluidSynthBackend() override {
        if (synth_ != nullptr && delete_synth_ != nullptr) {
            delete_synth_(synth_);
        }
        if (settings_ != nullptr && delete_settings_ != nullptr) {
            delete_settings_(settings_);
        }
        //never unload the dll: a render thread may still be running at shutdown
    }

    bool init(unsigned int sample_rate) {
        static const wchar_t* candidates[] = {L"libfluidsynth-3.dll", L"libfluidsynth-2.dll",
                                              L"fluidsynth.dll"};

        for (const wchar_t* name : candidates) {
            module_ = LoadLibraryW(name);
            if (module_ != nullptr) {
                Debugger::log("Found and loaded FluidSynth DLL");
                break;
            }

            const DWORD error = GetLastError();
            Debugger::log_error(std::format(
                "Failed to load {} (Win32 error {})",
                std::filesystem::path(name).string(),
                error
            ));
        }

        if (module_ == nullptr) {
            return false;
        }

        const auto load = [this](const char* name) {
            return GetProcAddress(module_, name);
        };

        auto new_settings = reinterpret_cast<new_fluid_settings_fn>(load("new_fluid_settings"));
        delete_settings_ =
            reinterpret_cast<delete_fluid_settings_fn>(load("delete_fluid_settings"));
        auto settings_setnum =
            reinterpret_cast<fluid_settings_setnum_fn>(load("fluid_settings_setnum"));
        auto settings_setint =
            reinterpret_cast<fluid_settings_setint_fn>(load("fluid_settings_setint"));
        auto new_synth = reinterpret_cast<new_fluid_synth_fn>(load("new_fluid_synth"));
        delete_synth_ = reinterpret_cast<delete_fluid_synth_fn>(load("delete_fluid_synth"));
        sfload_ = reinterpret_cast<fluid_synth_sfload_fn>(load("fluid_synth_sfload"));
        noteon_ = reinterpret_cast<fluid_synth_noteon_fn>(load("fluid_synth_noteon"));
        noteoff_ = reinterpret_cast<fluid_synth_noteoff_fn>(load("fluid_synth_noteoff"));
        cc_ = reinterpret_cast<fluid_synth_cc_fn>(load("fluid_synth_cc"));
        pitch_bend_ = reinterpret_cast<fluid_synth_pitch_bend_fn>(load("fluid_synth_pitch_bend"));
        all_notes_off_ =
            reinterpret_cast<fluid_synth_all_notes_off_fn>(load("fluid_synth_all_notes_off"));
        system_reset_ =
            reinterpret_cast<fluid_synth_system_reset_fn>(load("fluid_synth_system_reset"));
        write_float_ =
            reinterpret_cast<fluid_synth_write_float_fn>(load("fluid_synth_write_float"));

        if (new_settings == nullptr || new_synth == nullptr || write_float_ == nullptr ||
            sfload_ == nullptr) {
            Debugger::log_error("FluidSynth was found but is missing entry points.");
            return false;
        }

        settings_ = new_settings();
        if (settings_ == nullptr) {
            return false;
        }

        if (settings_setnum != nullptr) {
            settings_setnum(settings_, "synth.sample-rate", static_cast<double>(sample_rate));
            settings_setnum(settings_, "synth.gain", 0.6);
        }
        if (settings_setint != nullptr) {
            settings_setint(settings_, "synth.polyphony", 65536);
            settings_setint(settings_, "synth.cpu-cores", 4);
        }

        synth_ = new_synth(settings_);
        return synth_ != nullptr;
    }

    void note_on(std::uint8_t channel, std::uint8_t key, std::uint8_t velocity) override {
        if (noteon_ != nullptr) {
            noteon_(synth_, channel, key, velocity);
        }
    }

    void note_off(std::uint8_t channel, std::uint8_t key) override {
        if (noteoff_ != nullptr) {
            noteoff_(synth_, channel, key);
        }
    }

    void control(std::uint8_t channel, std::uint8_t controller, std::uint8_t value) override {
        if (cc_ != nullptr) {
            cc_(synth_, channel, controller, value);
        }
    }

    void pitch_bend(std::uint8_t channel, int bend) override {
        if (pitch_bend_ != nullptr) {
            pitch_bend_(synth_, channel, std::clamp(bend, 0, 16383));
        }
    }

    void all_notes_killed() override {
        if (all_notes_off_ != nullptr) {
            for (int channel = 0; channel < 16; ++channel) {
                all_notes_off_(synth_, channel);
            }
        }
    }

    void reset_control() override {
        if (system_reset_ != nullptr) {
            system_reset_(synth_);
        }
    }

    void read_samples(float* out, std::size_t frames) override {
        if (write_float_ == nullptr || frames == 0) {
            return;
        }
        write_float_(synth_, static_cast<int>(frames), out, 0, 2, out, 1, 2);
    }

    bool load_soundfonts(const std::vector<std::string>& paths) override {
        bool any = false;
        for (const std::string& path : paths) {
            const int id = sfload_(synth_, path.c_str(), 1);
            if (id == -1) {
                Debugger::log_error(std::format("Could not load soundfont {}", path));
            } else {
                Debugger::log(std::format("Loaded soundfont {}", path));
                any = true;
            }
        }
        return any;
    }

private:
    HMODULE module_ = nullptr;
    fluid_settings_t* settings_ = nullptr;
    fluid_synth_t* synth_ = nullptr;

    delete_fluid_settings_fn delete_settings_ = nullptr;
    delete_fluid_synth_fn delete_synth_ = nullptr;
    fluid_synth_sfload_fn sfload_ = nullptr;
    fluid_synth_noteon_fn noteon_ = nullptr;
    fluid_synth_noteoff_fn noteoff_ = nullptr;
    fluid_synth_cc_fn cc_ = nullptr;
    fluid_synth_pitch_bend_fn pitch_bend_ = nullptr;
    fluid_synth_all_notes_off_fn all_notes_off_ = nullptr;
    fluid_synth_system_reset_fn system_reset_ = nullptr;
    fluid_synth_write_float_fn write_float_ = nullptr;
};

#endif

}

std::unique_ptr<SoftSynth> create_soft_synth(unsigned int sample_rate) {
#ifdef _WIN32
    auto backend = std::make_unique<FluidSynthBackend>();
    if (!backend->init(sample_rate)) {
        Debugger::log_warning(
            "No software synthesiser available (libfluidsynth-3.dll was not found next to the "
            "executable), so prerendered audio is unavailable.");
        return nullptr;
    }
    return backend;
#else
    (void)sample_rate;
    return nullptr;
#endif
}

std::vector<std::string> default_soundfont_paths() {
    std::vector<std::string> paths;

    if (const char* env = std::getenv("ANDROMEDA_SOUNDFONT"); env != nullptr && *env != '\0') {
        paths.emplace_back(env);
        return paths;
    }

    std::error_code ec;
    const std::filesystem::path dir = "assets/soundfonts";
    if (!std::filesystem::exists(dir, ec)) {
        return paths;
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            break;
        }
        std::string ext = entry.path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (ext == ".sf2" || ext == ".sf3") {
            paths.push_back(entry.path().string());
        }
    }

    return paths;
}

}
