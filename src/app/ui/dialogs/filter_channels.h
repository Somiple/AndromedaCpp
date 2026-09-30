#pragma once

#include <array>
#include <memory>
#include <string>

#include "app/ui/dialog.h"

namespace andromeda::editor {
class NoteEditing;
class SharedSelectedNotes;
}

namespace andromeda::app {

class FilterChannelsDialog final : public Dialog {
public:
    std::expected<void, std::string> init_dialog(DialogArgs& args) override;

    MaybeDlgAction draw(const ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return dialog_names::DIALOG_NAME_FILTER_CHANNELS;
    }
    [[nodiscard]] std::string get_dialog_title() const override {
        return "Filter selection channels";
    }
    [[nodiscard]] std::optional<DialogActionButtons> get_action_buttons() const override;

private:
    void apply_filter();

    std::array<bool, 16> channels_filter_{};
    bool should_filter_ = false;
    std::shared_ptr<editor::SharedSelectedNotes> shared_selected_notes_;
    std::shared_ptr<editor::NoteEditing> note_editing_;
};

}
