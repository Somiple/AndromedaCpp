#pragma once

#include <string>

#include "app/ui/dialog.h"

namespace andromeda::editor {

class PluginErrorDialog final : public app::Dialog {
public:
    std::expected<void, std::string> init_dialog(app::DialogArgs& args) override;

    app::MaybeDlgAction draw(const app::ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return app::dialog_names::DIALOG_NAME_PLUGIN_ERROR_DIALOG;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Plugin Error!"; }
    [[nodiscard]] std::optional<app::DialogActionButtons> get_action_buttons() const override {
        return app::DlgOk{app::dialog_default_close_action()};
    }
    [[nodiscard]] std::uint16_t get_flags() const override {
        return app::dialog_flags::DIALOG_NO_COLLAPSABLE;
    }

private:
    std::string plugin_name_;
    std::string error_msg_;
};

}
