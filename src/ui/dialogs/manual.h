#pragma once

#include "ui/dialogs/dialog.h"

namespace andromeda::app {

/// The tabbed user manual.
class EditorManualDialog final : public Dialog {
public:
    [[nodiscard]] const char* get_title() const override { return "Andromeda Manual"; }
    void draw_content() override;

    [[nodiscard]] const DialogActions get_actions() const override { return DialogActions::Ok; }

private:
    enum class Section { Welcome, Navigating, CreatingNotess, EditingNotes, EditingTools, TimelineAndSnap };

    void draw_welcome_tab() const;
    void draw_navigating_tab() const;

    Section manual_section_ = Section::Welcome;
};

} // namespace andromeda::app
