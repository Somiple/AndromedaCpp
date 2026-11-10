#pragma once

#include <array>
#include <string>

#include "ui/dialogs/dialog.h"

namespace andromeda::app {

/// Post-mortem report box: shows the last panic and lets the user add context before the report
/// is sent and the app exits.
class CrashDialog : public Dialog {
public:
    const char* get_title() const override { return "Catastrophic Error"; }
    void draw_content() override;

    void on_open(const DialogPayload& payload) override;
    void on_click_confirm() override;
    /// refactor: the footer spec is part of the Dialog contract, so this is marked override like
    /// every other leaf dialog instead of silently shadowing it with a differently-spelled method.
    [[nodiscard]] const DialogActions get_actions() const override { return DialogActions::Ok; }
private:
    void send_report() const;

    std::string last_exception;
    std::array<char, 128> username;
    std::array<char, 1024> details;
};

}
