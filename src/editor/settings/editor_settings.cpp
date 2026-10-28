#include "editor/settings/editor_settings.h"

#include <mutex>
#include <utility>

#include <imgui.h>

#include "ui/dialogs/ui_helpers.h"
#include "audio/midi_audio.h"
#include "core/debugger.h"

namespace andromeda::editor {

using app::DisabledScope;
using app::selectable_label;
using app::text_sized;
using app::vertical_separator;
using util::Debugger;

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
    // reused from comet
    if (!audio_engine_) return;

    const auto& engines = audio_engine_->get_engine_list();
    
    for (size_t i = 0; i < engines.size(); i++) {
        const auto* engine = engines[i].get();
        if (!engine) continue;

        if (ImGui::Selectable(engine->get_name().c_str(), audio_engine_->get_current_engine_type() == (audio::AudioEngineType)i)) {
            audio_engine_->switch_engine((audio::AudioEngineType)i);
        }
    }

    audio::AudioEngine* curr_engine = audio_engine_->get_current_engine();
    if (!curr_engine) return;
    curr_engine->render_settings();
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
