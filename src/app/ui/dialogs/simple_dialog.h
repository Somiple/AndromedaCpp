#pragma once

#include <string>

#include "app/ui/dialog.h"

namespace andromeda::app {

class SimpleDialog final : public Dialog {
public:
    std::expected<void, std::string> init_dialog(DialogArgs& args) override;

    MaybeDlgAction draw(const ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return dialog_names::DIALOG_NAME_SIMPLE;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return title_; }
    [[nodiscard]] std::optional<DialogActionButtons> get_action_buttons() const override;

    std::string id;
    bool ok_clicked = false;

private:
    std::string title_;
    std::string msg_;
    bool is_yesno_ = true;
};

}
