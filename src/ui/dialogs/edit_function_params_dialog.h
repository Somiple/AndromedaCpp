#pragma once

// ---------------------------------------------------------------------------
// Layer: ui
// One generic parameters dialog for every edit function that declares inputs.
// It knows no concrete command: it renders the EditParam list it is handed and
// hands the collected values back through DialogRegistry's on_confirmed hook.
// See docs/ARCHITECTURE.md section 13.
// ---------------------------------------------------------------------------

#include <string>
#include <vector>

#include "editor/edit_function_registry.h"
#include "ui/dialogs/dialog.h"

namespace andromeda::app {

/// Payload for EditFunctionParamsDialog::on_open.
/// refactor: replaces the old EditFunctionDialogArgs, which was pushed into a
/// std::vector<std::any> alongside unrelated dialogs' arguments. One struct, one payload.
struct EditFunctionDialogOptions {
    std::string function_id;                ///< registry id the values belong to
    std::string title;                      ///< window title (the command label)
    std::vector<editor::EditParam> params;  ///< declared inputs, rendered top to bottom
    editor::EditFunctionArgs values;        ///< initial values, normally the declared defaults
};

/// Renders any edit function's declared parameters. Command-agnostic by construction.
class EditFunctionParamsDialog : public Dialog {
public:
    [[nodiscard]] const char* get_title() const override {
        return m_title.c_str();
    }

    void draw_content() override;

    /// @param payload must hold an EditFunctionDialogOptions; a mismatch is logged and leaves
    ///                the previous function's values untouched instead of crashing.
    void on_open(const DialogPayload& payload) override;

    [[nodiscard]] const DialogActions get_actions() const override { return DialogActions::OkCancel; }

    /// @return the registry id the collected values belong to.
    [[nodiscard]] const std::string& function_id() const noexcept { return m_function_id; }
    /// @return the values as edited by the user.
    [[nodiscard]] const editor::EditFunctionArgs& values() const noexcept { return m_values; }
private:
    std::string m_function_id;
    std::string m_title;
    std::vector<editor::EditParam> m_params;
    editor::EditFunctionArgs m_values;
};

} // namespace andromeda::app
