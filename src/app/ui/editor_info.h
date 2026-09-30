#pragma once

#include <string>

#include "app/ui/dialog.h"

namespace andromeda::app {

class EditorInfo final : public Dialog {
public:
    MaybeDlgAction draw(const ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return dialog_names::DIALOG_NAME_EDITOR_INFO;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Editor Info"; }
    [[nodiscard]] std::optional<DialogActionButtons> get_action_buttons() const override {
        return DlgOk{dialog_default_close_action()};
    }
    [[nodiscard]] std::uint16_t get_flags() const override {
        return dialog_flags::DIALOG_NO_COLLAPSABLE | dialog_flags::DIALOG_NO_RESIZABLE;
    }
};

}
