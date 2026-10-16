#include "app/widgets/data_view_widget.h"

#include <utility>

#include <imgui.h>

#include "app/main_window.h"
#include "app/rendering/data_view.h"

namespace andromeda::app {

DataViewWidget::DataViewWidget(MainWindow& parent,
                               std::shared_ptr<rendering::NoteGpuCache> note_cache)
    : EditorWidget(parent), data_editing_(parent.editor_controller.get_data_editing()) {
    auto renderer = std::make_shared<rendering::DataViewRenderer>(&parent);
    renderer->use_note_cache(std::move(note_cache));

    target_renderer = std::move(renderer);
    component = data_editing_;
}

std::shared_ptr<rendering::DataViewRenderer> DataViewWidget::renderer() const {
    return std::static_pointer_cast<rendering::DataViewRenderer>(target_renderer);
}

void DataViewWidget::handle_input() {
    using namespace editor::data_edit_flags;

    const bool pointer_in_panel = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if (!pointer_in_panel) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            data_editing_->disable_flag(DATA_EDIT_CLICKED_IN_RECT | DATA_EDIT_DRAW_EDIT_LINE);
        }
        return;
    }

    data_editing_->set_flag(DATA_EDIT_MOUSE_OVER_UI, context.parent->mouse_over_ui);
    data_editing_->set_flag(DATA_EDIT_ANY_DIALOG_OPEN, false);

    data_editing_->update();

    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        data_editing_->on_mouse_down();
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        data_editing_->on_mouse_move();
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        data_editing_->on_mouse_up();
    }
}

void DataViewWidget::draw_overlays() {
    if (!data_editing_->get_flag(editor::data_edit_flags::DATA_EDIT_DRAW_EDIT_LINE)) {
        return;
    }

    const auto [pt1, pt2] = data_editing_->get_data_view_line_points();
    ImGui::GetWindowDrawList()->AddLine({pt1.x, pt1.y}, {pt2.x, pt2.y},
                                        IM_COL32(255, 255, 255, 255), 1.0f);
}

}
