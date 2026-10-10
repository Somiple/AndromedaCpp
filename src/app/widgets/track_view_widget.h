#pragma once

#include "app/widgets/editor_widget.h"
#include "editor/editing/track_editing.h"

namespace andromeda::app {

class TrackViewWidget final : public EditorWidget {
public:
    // must run after glad has loaded the gl functions
    explicit TrackViewWidget(MainWindow& parent);

    [[nodiscard]] TickView tick_view() const override;
    void jump_to_tick(float pos) override;
    void zoom_by(float x_fac, float y_fac) override;

protected:
    void handle_navigation() override;
    void handle_input() override;
    void draw_overlays() override;

private:
    void draw_select_box() const;
    void draw_context_menu();

    editor::TrackEditing* track_editing_ = nullptr;
};

}
