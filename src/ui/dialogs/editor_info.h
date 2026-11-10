#pragma once

#include "ui/dialogs/dialog.h"

namespace andromeda::app {

/// About box: version and credits.
class EditorInfo final : public Dialog {
public:
    [[nodiscard]] const char* get_title() const override { return "About Andromeda"; }
    void draw_content() override;

    [[nodiscard]] const DialogActions get_actions() const override { return DialogActions::Ok; }
};

} // namespace andromeda::app
