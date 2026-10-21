#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include "app/custom_widgets.h"
#include "app/ui/dialog.h"

namespace andromeda::audio {
class MIDIAudio;
class AudioEngine;
}

namespace andromeda::editor {

inline constexpr float PR_KEYBOARD_WIDTH = 100.0f;

class Settings {
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
};

class ESSettingsWindow final : public app::Dialog {
public:

    void set_audio_engine(audio::MIDIAudio* engine) { audio_engine_ = engine; }

    // fixed rust bug: the window always opened on its defaults, not on the engine that plays
    void set_active_engine(ESAudioEngineType engine) { audio_settings_.md_engine = engine; }

    app::MaybeDlgAction draw(const app::ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_EDITOR_SETTINGS;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Editor Settings"; }
    [[nodiscard]] std::uint16_t get_flags() const override {
        return app::dialog_flags::DIALOG_NO_COLLAPSABLE;
    }
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override {
        return app::DlgOk{app::dialog_default_close_action()};
    }

    [[nodiscard]] const ESGeneralSettings& general_settings() const { return general_settings_; }
    [[nodiscard]] const ESAudioSettings& audio_settings() const { return audio_settings_; }

private:
    enum class Tab { General, Audio };

    void draw_general_tab();
    void draw_audio_tab();

    bool is_shown_ = false;
    Tab curr_settings_ = Tab::General;
    ESGeneralSettings general_settings_;
    ESAudioSettings audio_settings_;

    audio::MIDIAudio* audio_engine_ = nullptr;
};

}
