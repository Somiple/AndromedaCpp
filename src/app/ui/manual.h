#pragma once

#include <string>

#include "app/ui/dialog.h"

namespace andromeda::app {

class EditorManualDialog final : public Dialog {
public:
    MaybeDlgAction draw(const ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return dialog_names::DIALOG_NAME_EDITOR_MANUAL;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Andromeda Manual"; }
    [[nodiscard]] std::optional<DialogActionButtons> get_action_buttons() const override {
        return DlgOk{[](Dialog&) -> MaybeDlgAction {
            return DialogClose{dialog_names::DIALOG_NAME_EDITOR_MANUAL};
        }};
    }
    [[nodiscard]] std::uint16_t get_flags() const override {
        return dialog_flags::DIALOG_NO_COLLAPSABLE | dialog_flags::DIALOG_NO_RESIZABLE;
    }

private:
    enum class Section { Welcome, Navigating };

    void draw_welcome_tab() const;
    void draw_navigating_tab() const;

    Section manual_section_ = Section::Welcome;
};

}
