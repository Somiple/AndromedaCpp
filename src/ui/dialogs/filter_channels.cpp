#include "ui/dialogs/filter_channels.h"

#include <format>
#include <shared_mutex>
#include <utility>
#include <vector>

#include <imgui.h>

#include "editor/editing.h"
#include "editor/editing/note_editing.h"
#include "midi/midi_track.h"
#include "editor/editor_controller.h"
#include "core/debugger.h"

namespace andromeda::app {

void FilterChannelsDialog::on_open(const DialogPayload& payload) {
    const auto* options = payload.get_if<FilterChannelsDialogOptions>();
    if (options == nullptr || options->controller == nullptr) {
        util::Debugger::log_error(
            "FilterChannelsDialog opened without a controller in its payload");
        _controller = nullptr;
        return;
    }

    channels_filter_.fill(false);
    should_filter_ = false;
    _controller = options->controller;
}

void FilterChannelsDialog::draw_content() {
    for (std::size_t chan = 0; chan < 8; ++chan) {
        if (chan > 0) {
            ImGui::SameLine();
        }
        ImGui::Checkbox(std::format("{}", chan).c_str(), &channels_filter_[chan]);
    }
    for (std::size_t chan = 0; chan < 8; ++chan) {
        if (chan > 0) {
            ImGui::SameLine();
        }
        ImGui::Checkbox(std::format("{}", chan + 8).c_str(), &channels_filter_[chan + 8]);
    }
}

// refactor: the apply button used to run this through a DlgApplyClose callback; the base now
// routes a confirm straight into on_click_confirm, so the same work happens in one fewer hop.
void FilterChannelsDialog::on_click_confirm() {
    apply_filter();
}

void FilterChannelsDialog::apply_filter() {
    if (!_controller) {
        return;
    }
    editor::SharedSelection* selection = _controller->get_note_selection();

    should_filter_ = true;

    auto* tracks = _controller->get_project_manager()->get_tracks();
    if (!tracks) {
        return;
    }

    const std::vector<std::uint16_t> active_tracks = selection->get_active_selected_tracks();

    for (const std::uint16_t track : active_tracks) {
        if (track >= tracks->size()) {
            continue;
        }
        const std::vector<midi::Note>& notes = tracks->at(track).get_notes();

        const std::vector<std::size_t>* selected = selection->get_selected_ids_in_track(track);
        if (!selected) {
            continue;
        }

        std::vector<std::size_t> kept_ids;
        for (const std::size_t id : *selected) {
            if (id >= notes.size()) {
                continue;
            }
            const std::uint8_t channel = notes[id].get_channel();
            if (channels_filter_[channel]) {
                kept_ids.push_back(id);
            }
        }

        selection->set_selected_in_track(std::move(kept_ids), track);
    }
}

} // namespace andromeda::app
