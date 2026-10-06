#include "app/ui/dialogs/filter_channels.h"

#include <format>
#include <shared_mutex>
#include <vector>

#include <imgui.h>

#include "editor/editing.h"
#include "editor/editing/note_editing.h"
#include "midi/midi_track.h"
#include "editor/editor_controller.h"

namespace andromeda::app {

std::expected<void, std::string> FilterChannelsDialog::init_dialog(DialogArgs& args) {
    if (args.size() < 2) {
        return std::unexpected("Filter channels needs the selection and the note editor.");
    }

    auto* controller = std::any_cast<editor::EditorController*>(&args[0]);
    if (!controller) {
        return std::unexpected("Filter channels was given the wrong argument types.");
    }

    channels_filter_.fill(false);
    should_filter_ = false;
    _controller = *controller;

    return {};
}

MaybeDlgAction FilterChannelsDialog::draw(const ImageResources&) {
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

    return std::nullopt;
}

void FilterChannelsDialog::apply_filter() {
    if (!_controller) {
        return;
    }
    editor::SharedSelectedNotes* selection = _controller->get_selection();

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

std::optional<DialogActionButtons> FilterChannelsDialog::get_action_buttons() const {
    return DlgApplyClose{[](Dialog& dlg) -> MaybeDlgAction {
                             auto& self = static_cast<FilterChannelsDialog&>(dlg);
                             self.apply_filter();
                             return DialogClose{self.get_dialog_name()};
                         },
                         dialog_default_close_action()};
}

}
