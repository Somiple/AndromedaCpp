#include "app/ui/dialogs/filter_channels.h"

#include <format>
#include <shared_mutex>
#include <vector>

#include <imgui.h>

#include "editor/editing.h"
#include "editor/editing/note_editing.h"
#include "midi/midi_track.h"

namespace andromeda::app {

std::expected<void, std::string> FilterChannelsDialog::init_dialog(DialogArgs& args) {
    if (args.size() < 2) {
        return std::unexpected("Filter channels needs the selection and the note editor.");
    }

    auto* selected = std::any_cast<std::shared_ptr<editor::SharedSelectedNotes>>(&args[0]);
    auto* note_editing = std::any_cast<std::shared_ptr<editor::NoteEditing>>(&args[1]);
    if (!selected || !note_editing) {
        return std::unexpected("Filter channels was given the wrong argument types.");
    }

    channels_filter_.fill(false);
    should_filter_ = false;
    shared_selected_notes_ = *selected;
    note_editing_ = *note_editing;

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
    if (!shared_selected_notes_ || !note_editing_) {
        return;
    }

    should_filter_ = true;

    const auto tracks = note_editing_->get_tracks();
    if (!tracks) {
        return;
    }

    std::shared_lock lock(tracks->mutex);

    const std::vector<std::uint16_t> active_tracks =
        shared_selected_notes_->get_active_selected_tracks();

    for (const std::uint16_t track : active_tracks) {
        if (track >= tracks->value.size()) {
            continue;
        }
        const std::vector<midi::Note>& notes = tracks->value[track].get_notes();

        const std::vector<std::size_t>* selected =
            shared_selected_notes_->get_selected_ids_in_track(track);
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

        shared_selected_notes_->set_selected_in_track(std::move(kept_ids), track);
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
