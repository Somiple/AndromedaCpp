#include "editor/settings/editor_settings.h"

#include <mutex>
#include <utility>

#include <imgui.h>

#include "app/ui/ui_helpers.h"
#include "audio/midi_devices.h"
#include "util/debugger.h"

namespace andromeda::editor {

using app::DisabledScope;
using app::selectable_label;
using app::text_sized;
using app::vertical_separator;
using util::Debugger;

void ESSettingsWindow::use_midi_devices(std::shared_ptr<audio::MIDIDevices> devices,
                                        SharedDevice slot) {
    midi_devices_ = std::move(devices);
    midi_devices_slot_ = std::move(slot);
}

void ESSettingsWindow::use_kdmapi(SharedDevice slot) { kdmapi_slot_ = std::move(slot); }

void ESSettingsWindow::use_playback_manager(std::shared_ptr<audio::AudioEngine> playback_manager) {
    playback_manager_ = std::move(playback_manager);
}

void ESSettingsWindow::draw_general_tab() {
    ESGeneralSettings& gs = general_settings_;

    text_sized(15.0f, "MIDI Import");
    {
        ImGui::Checkbox("Discard empty tracks", &gs.import_discard_empty_tracks);
        {
            DisabledScope scope(gs.import_discard_empty_tracks);
            ImGui::Checkbox("Keep empty tracks containing non-note events",
                            &gs.import_keep_empty_with_cc);
        }

        ImGui::Checkbox("Reassign channels", &gs.import_reassign_channels);
        ImGui::Checkbox("Reassign channel 10 to channel 11",
                        &gs.import_reassign_channel_10_as_11);

        ImGui::Checkbox("Keep PPQ at a Maximum", &gs.import_max_ppq_override);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip(
                "If any imported MIDI's PPQ exceeds the specified PPQ, the MIDI will be "
                "quantized.");
        }
        {
            DisabledScope scope(gs.import_max_ppq_override);
            gs.import_max_ppq_override_value.show("Max PPQ");
        }

        ImGui::Checkbox("Remove overlaps", &gs.import_remove_overlaps);
    }
    ImGui::Separator();
    text_sized(15.0f, "MIDI Export");
    { ImGui::Checkbox("Discard empty tracks##export", &gs.export_discard_empty_tracks); }
}

void ESSettingsWindow::draw_audio_tab() {
    // fixed rust bug: clicking the engine already in use reopened it and reset its port
    if (selectable_label(audio_settings_.md_engine == ESAudioEngineType::MidiIO, "MIDI I/O") &&
        audio_settings_.md_engine != ESAudioEngineType::MidiIO) {
        audio_settings_.md_engine = ESAudioEngineType::MidiIO;

        if (playback_manager_ && midi_devices_slot_) {
            playback_manager_->switch_device(midi_devices_slot_);
        }
    }
    ImGui::SameLine();

    if (selectable_label(audio_settings_.md_engine == ESAudioEngineType::KDMAPI, "KDMAPI") &&
        audio_settings_.md_engine != ESAudioEngineType::KDMAPI) {
        audio_settings_.md_engine = ESAudioEngineType::KDMAPI;

        if (playback_manager_ && kdmapi_slot_) {
            playback_manager_->switch_device(kdmapi_slot_);
        }
    }
    ImGui::SameLine();

    {
        DisabledScope scope(false);
        if (selectable_label(audio_settings_.md_engine == ESAudioEngineType::Prerendered,
                             "Prerendered Audio (startup only: ANDROMEDA_AUDIO_ENGINE=prerender)")) {
            audio_settings_.md_engine = ESAudioEngineType::Prerendered;
        }
    }

    ImGui::Separator();

    switch (audio_settings_.md_engine) {
    case ESAudioEngineType::MidiIO:
        draw_audio_tab_midi_io();
        break;
    case ESAudioEngineType::KDMAPI:
    case ESAudioEngineType::Prerendered:
        draw_audio_tab_kdmapi();
        break;
    }
}

void ESSettingsWindow::draw_audio_tab_midi_io() {
    if (!midi_devices_) {
        return;
    }

    // the highlighted ports are the ones actually open, not this window's defaults
    text_sized(15.0f, "MIDI Input Devices");
    {
        const std::vector<std::string>& midi_in_names = midi_devices_->get_midi_in_port_names();
        for (std::size_t i = 0; i < midi_in_names.size(); ++i) {
            if (selectable_label(midi_devices_->get_curr_in_port() == i, midi_in_names[i].c_str())) {
                audio_settings_.md_port_in = i;
                std::unique_lock<std::mutex> lock;
                if (midi_devices_slot_) {
                    lock = std::unique_lock(midi_devices_slot_->mutex);
                }
                if (auto result = midi_devices_->connect_in_port(i); !result) {
                    Debugger::log_error(result.error());
                }
            }
        }
    }
    ImGui::Separator();
    text_sized(15.0f, "MIDI Output Devices");
    {
        const std::vector<std::string>& midi_out_names = midi_devices_->get_midi_out_port_names();
        for (std::size_t i = 0; i < midi_out_names.size(); ++i) {
            if (selectable_label(midi_devices_->get_curr_out_port() == i,
                                 midi_out_names[i].c_str())) {
                audio_settings_.md_port_out = i;
                // the playback thread sends through this port under the same lock
                std::unique_lock<std::mutex> lock;
                if (midi_devices_slot_) {
                    lock = std::unique_lock(midi_devices_slot_->mutex);
                }
                if (auto result = midi_devices_->connect_out_port(i); !result) {
                    Debugger::log_error(result.error());
                }
            }
        }
    }
    ImGui::Separator();
    text_sized(15.0f, "Advanced");
    {
        audio_settings_.md_event_pool_size.show("MIDI Event pool size");
        if (audio_settings_.md_event_pool_size.changed) {
        }
    }
}

void ESSettingsWindow::draw_audio_tab_kdmapi() {
    text_sized(15.0f, "KDMAPI");
    ImGui::Separator();
    ImGui::TextUnformatted("Open OmniMIDI to adjust settings.");
}

app::MaybeDlgAction ESSettingsWindow::draw(const app::ImageResources&) {
    ImGui::BeginGroup();
    if (selectable_label(curr_settings_ == Tab::General, "General")) {
        curr_settings_ = Tab::General;
    }
    if (selectable_label(curr_settings_ == Tab::Audio, "Audio")) {
        curr_settings_ = Tab::Audio;
    }
    ImGui::EndGroup();

    ImGui::SameLine();
    vertical_separator();
    ImGui::SameLine();

    ImGui::BeginChild("##settings_body", ImVec2(420.0f, 320.0f));
    switch (curr_settings_) {
    case Tab::General:
        draw_general_tab();
        break;
    case Tab::Audio:
        draw_audio_tab();
        break;
    }
    ImGui::EndChild();

    return std::nullopt;
}

}
