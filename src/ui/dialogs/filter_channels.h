#pragma once

#include <array>

#include "ui/dialogs/dialog.h"

namespace andromeda::editor {
class EditorController;
}

namespace andromeda::app {

/// Payload for FilterChannelsDialog::on_open.
/// refactor: replaces the `std::vector<std::any>` pair (controller*, note editor*) — only the
/// controller was ever used, so the payload carries exactly that.
struct FilterChannelsDialogOptions {
    editor::EditorController* controller = nullptr;  ///< not owned; must outlive the dialog
};

/// Keeps only the notes whose channel is ticked and drops the rest from the selection.
class FilterChannelsDialog final : public Dialog {
public:
    [[nodiscard]] const char* get_title() const override {
        return "Filter selection channels";
    }

    void draw_content() override;

    /// @param payload must hold a FilterChannelsDialogOptions carrying a controller.
    void on_open(const DialogPayload& payload) override;

    /// Applies the ticked channels to the current selection.
    void on_click_confirm() override;

    [[nodiscard]] const DialogActions get_actions() const override {
        return DialogActions::OkCancel;
    }

private:
    void apply_filter();

    std::array<bool, 16> channels_filter_{};
    bool should_filter_ = false;
    editor::EditorController* _controller = nullptr;
};

} // namespace andromeda::app
