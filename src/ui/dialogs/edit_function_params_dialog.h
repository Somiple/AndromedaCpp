#pragma once

// ---------------------------------------------------------------------------
// Layer: ui
// One generic parameters dialog for every edit function that declares inputs.
// It knows no concrete command: it renders the EditParam list it is handed and
// hands the collected values back through DialogCoordinator.
// See docs/ARCHITECTURE.md section 13.
// ---------------------------------------------------------------------------

#include <expected>
#include <optional>
#include <string>
#include <vector>

#include "editor/edit_function_registry.h"
#include "ui/dialogs/dialog.h"

namespace andromeda::app {

/// Type-erased payload passed through DialogArgs when the parameters dialog is opened.
struct EditFunctionDialogArgs {
    std::string function_id;                ///< registry id the values belong to
    std::string title;                      ///< window title (the command label)
    std::vector<editor::EditParam> params;  ///< declared inputs, rendered top to bottom
    editor::EditFunctionArgs values;        ///< initial values, normally the declared defaults
};

/// Renders any edit function's declared parameters. Command-agnostic by construction.
class EditFunctionParamsDialog final : public Dialog {
public:
    /// Reads an EditFunctionDialogArgs out of the type-erased argument vector.
    /// @param args must contain exactly one EditFunctionDialogArgs.
    /// @return an error message when the payload is missing or malformed.
    std::expected<void, std::string> init_dialog(DialogArgs& args) override;

    MaybeDlgAction draw(const ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return dialog_names::DIALOG_NAME_EDIT_FUNCTION_PARAMS;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return m_title; }
    [[nodiscard]] std::optional<DialogActionButtons> get_action_buttons() const override;

    /// @return the registry id the collected values belong to.
    [[nodiscard]] const std::string& function_id() const noexcept { return m_function_id; }
    /// @return the values as edited by the user.
    [[nodiscard]] const editor::EditFunctionArgs& values() const noexcept { return m_values; }

    bool ok_clicked = false;  ///< set by the OK button, read by DialogCoordinator

private:
    std::string m_function_id;
    std::string m_title;
    std::vector<editor::EditParam> m_params;
    editor::EditFunctionArgs m_values;
};

} // namespace andromeda::app
