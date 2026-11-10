#pragma once

#include <string>

#include "ui/dialogs/dialog.h"

namespace andromeda::editor {

/// Payload for PluginErrorDialog::on_open.
/// refactor: replaces the two-element `std::vector<std::any>` (name, message) the old
/// init_dialog unpacked positionally.
struct PluginErrorOptions {
    std::string plugin_name;  ///< plugin that failed
    std::string error_msg;    ///< message shown to the user
};

class PluginErrorDialog final : public app::Dialog {
public:
    [[nodiscard]] const char* get_title() const override { return "Plugin Error!"; }
    void draw_content() override;

    /// @param payload must hold a PluginErrorOptions; a mismatch is logged.
    void on_open(const app::DialogPayload& payload) override;

    [[nodiscard]] const app::DialogActions get_actions() const override {
        return app::DialogActions::Ok;
    }

private:
    std::string plugin_name_;
    std::string error_msg_;
};

}
