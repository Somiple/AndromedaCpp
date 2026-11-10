#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "ui/widgets/custom_widgets.h"
#include "ui/dialogs/dialog.h"

namespace andromeda::config {
struct EditorConfig;
}

namespace andromeda::audio {
class MIDIAudio;
class AudioEngine;
}

namespace andromeda::editor {

inline constexpr float PR_KEYBOARD_WIDTH = 100.0f;

/*class Settings {
public:
    virtual ~Settings() = default;
};

struct ESGeneralSettings final : Settings {
    bool import_discard_empty_tracks = false;
    bool import_keep_empty_with_cc = true;
    bool import_reassign_channels = false;
    bool import_reassign_channel_10_as_11 = false;
    bool import_max_ppq_override = false;
    app::NumericField<std::uint16_t> import_max_ppq_override_value{960, 96, 7680};
    bool import_remove_overlaps = false;

    bool export_discard_empty_tracks = true;
};

enum class ESAudioEngineType { MidiIO, KDMAPI, Prerendered };

struct ESAudioSettings final : Settings {
    ESAudioEngineType md_engine = ESAudioEngineType::Prerendered;
    std::size_t md_port_in = 0;
    std::size_t md_port_out = 0;

    app::NumericField<std::size_t> md_event_pool_size{4096, 100, 262144};

    [[nodiscard]] ESAudioEngineType get_engine() const { return md_engine; }
};*/

class ESSettingsWindow final : public app::Dialog {
public:
    void init(config::EditorConfig* config);
    void set_audio_engine(audio::MIDIAudio* engine) { audio_engine_ = engine; }

    [[nodiscard]] const char* get_title() const override { return "Editor Settings"; }
    void draw_content() override;

    [[nodiscard]] const app::DialogActions get_actions() const override {
        return app::DialogActions::Ok;
    }

private:
    enum class Tab { General, Audio };

    void draw_general_tab();
    void draw_audio_tab();

    bool is_shown_ = false;
    Tab curr_settings_ = Tab::General;

    audio::MIDIAudio* audio_engine_ = nullptr;
    config::EditorConfig* config_ = nullptr;
};

}
