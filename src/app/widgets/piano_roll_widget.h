#pragma once

#include "app/widgets/editor_widget.h"
#include "editor/editing/note_editing.h"

namespace andromeda::app {

namespace rendering {
class PianoRollRenderer;
}

class PianoRollWidget final : public EditorWidget {
public:
    // must run after glad has loaded the gl functions
    explicit PianoRollWidget(MainWindow& parent);

    [[nodiscard]] rendering::PianoRollRenderer& renderer() const;

    [[nodiscard]] TickView tick_view() const override;
    void jump_to_tick(float pos) override;
    void zoom_by(float x_fac, float y_fac) override;
    [[nodiscard]] float keyboard_width() const override;

protected:
    void handle_navigation() override;
    void handle_input() override;
    void draw_overlays() override;

private:
    void draw_select_box() const;

    editor::NoteEditing* note_editing_ = nullptr;
};

}
