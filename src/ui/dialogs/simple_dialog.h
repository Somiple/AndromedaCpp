#pragma once

#include <string>

#include "ui/dialogs/dialog.h"

namespace andromeda::app {

/// Payload for SimpleDialog::on_open.
/// refactor: replaces the positional `std::vector<std::any>` (title, message, id, yes/no) that
/// init_dialog used to unpack — the order was load-bearing and a missing element was an error.
struct SimpleDialogOptions {
    std::string title = "Andromeda";
    std::string message;
    std::string id;                                  ///< optional caller-side identity
    DialogActions actions = DialogActions::YesNo;    ///< footer buttons to present
};

/// A message box: one line of text and a caller-chosen button set.
class SimpleDialog final : public Dialog {
public:
    [[nodiscard]] const char* get_title() const override { return title_.c_str(); }
    void draw_content() override;

    /// @param payload must hold a SimpleDialogOptions; a mismatch is logged and the previous
    ///                message is left in place instead of being read as the wrong type.
    void on_open(const DialogPayload& payload) override;

    void on_click_confirm() override { ok_clicked = true; }

    [[nodiscard]] const DialogActions get_actions() const override { return actions_; }

    std::string id;
    bool ok_clicked = false;

private:
    std::string title_ = "Andromeda";
    std::string msg_;
    DialogActions actions_ = DialogActions::YesNo;
};

} // namespace andromeda::app
