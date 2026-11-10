#include "editor/settings/editor_settings.h"

#include <mutex>
#include <utility>

#include <imgui.h>

#include "ui/dialogs/ui_helpers.h"
#include "audio/midi_audio.h"
#include "core/debugger.h"
#include "ui/app/main_window.h"
#include "config/editor_config.h"
#include "ui/widgets/dialog_forum.h"

namespace {
using andromeda::ui::DialogForum;
}

namespace andromeda::editor {

using app::DisabledScope;
using app::selectable_label;
using app::text_sized;
using app::vertical_separator;
using util::Debugger;

void ESSettingsWindow::init(config::EditorConfig* config) {
    if (!config) {
        Debugger::log_error("Missing editor config pointer in settings dialog");
        return;
    }
    config_ = config;
}

void ESSettingsWindow::draw_general_tab() {
    if (!config_) return;

    DialogForum content("midi_tab");

    // ------------------------------------

    content.forum_header("MIDI Import");
    content.draw_field("##discard_empty", "Discard empty tracks", [&]() {
        ImGui::Checkbox("", &config_->midi.import_settings.discard_empty_tracks);
    });

    {
        DisabledScope scope(&config_->midi.import_settings.discard_empty_tracks);
        content.draw_field("##keep_empty", "Keep empty tracks containing non-note events", [&]() {
            ImGui::Checkbox("", &config_->midi.import_settings.keep_empty_with_cc);
        });
    }

    content.draw_field("##reassign", "Reassign Channel 10 to 11", [&]() {
        ImGui::Checkbox("", &config_->midi.import_settings.reassign_channel_10);
    });
    content.draw_field("##remove_overlap", "Remove overlaps", [&]() {
        ImGui::Checkbox("", &config_->midi.import_settings.remove_overlaps);
    });

    // ------------------------------------

    content.forum_header("MIDI Export");
    content.draw_field("##discard_empty_exp", "Discard empty tracks", [&]() {
        ImGui::Checkbox("", &config_->midi.export_settings.discard_empty_tracks);
    });
    content.draw_field("##ignore_muted", "Ignore muted tracks", [&]() {
        ImGui::Checkbox("", &config_->midi.export_settings.ignore_muted_tracks);
        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("Does not include muted tracks when exporting MIDIs.");
        }
    });
    content.draw_field("##export_threads", "Threads", [&]() {
        int export_threads = config_->midi.export_settings.threads;
        if (ImGui::InputInt("", &export_threads, 1, 0)) {
            int max_threads = std::thread::hardware_concurrency();
            if (export_threads > max_threads) export_threads = max_threads;
            if (export_threads < 0) export_threads = 0;
            config_->midi.export_settings.threads = export_threads;
        }

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("0 thread = automatic. Number of threads to use when exporting MIDI files.");
        }
    });
}

void ESSettingsWindow::draw_audio_tab() {
    // reused from comet
    if (!audio_engine_) return;

    const auto& engines = audio_engine_->get_engine_list();
    
    ImGui::Text("Engine");
    ImGui::SameLine();

    if (ImGui::BeginCombo("##engine", audio_engine_->get_current_engine()->get_name().c_str())) {
        for (size_t i = 0; i < engines.size(); i++) {
            const auto* engine = engines[i].get();
            if (!engine) continue;

            if (ImGui::Selectable(engine->get_name().c_str(), audio_engine_->get_current_engine_type() == (audio::AudioEngineType)i)) {
                audio_engine_->switch_engine((audio::AudioEngineType)i);
                config_->audio.audio_engine_id = i;
            }
        }
        ImGui::EndCombo();
    }
    
    audio::AudioEngine* curr_engine = audio_engine_->get_current_engine();
    if (!curr_engine) return;

    ImGui::Spacing();
    ImGui::Text("Engine settings");
    if (ImGui::BeginChild("##audioEngineSettings", ImVec2(0.0f, 0.0f), ImGuiChildFlags_Borders)) {
        curr_engine->render_settings();
    }
    ImGui::EndChild();
}

void ESSettingsWindow::draw_content() {
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
}

}
