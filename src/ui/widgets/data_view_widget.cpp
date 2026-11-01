#include "ui/widgets/data_view_widget.h"

#include <utility>

#include <imgui.h>

#include "ui/app/main_window.h"
#include "ui/rendering/data_view.h"
#include "editor/editing/data_editing/event_lane.h"

namespace {
using andromeda::editor::EventSources;
}

namespace andromeda::app {

DataViewWidget::DataViewWidget(MainWindow& parent,
                               std::shared_ptr<rendering::NoteGpuCache> note_cache)
    : EditorWidget(parent), data_editing_(parent.controller().get_data_editing()) {
    // refactor: the local cannot be named `renderer`, or it shadows the accessor called below
    auto created_renderer = std::make_shared<rendering::DataViewRenderer>(&parent);
    created_renderer->use_note_cache(std::move(note_cache));

    target_renderer = std::move(created_renderer);
    component = data_editing_;

    event_lane_ = std::make_unique<editor::EventLane>(data_editing_);

    // the renderer draws whatever lane the editor projects, so hand it over once and let
    // refresh_lane() keep it current from here on
    if (const auto view = this->renderer(); view) {
        view->set_lane(event_lane_.get());
    }
    refresh_lane();
}

DataViewWidget::~DataViewWidget() = default;

std::shared_ptr<rendering::DataViewRenderer> DataViewWidget::renderer() const {
    return std::static_pointer_cast<rendering::DataViewRenderer>(target_renderer);
}

void DataViewWidget::refresh_lane() {
    if (event_lane_ == nullptr || data_editing_ == nullptr) {
        return;
    }

    const std::uint64_t key = lane_key();
    if (key == lane_key_ && !data_editing_->has_edited_recently()) {
        return;
    }

    event_lane_->rebuild_lane();
    event_lane_->rebuild_ghosts(onion_tracks(data_editing_->lane_track()));
    lane_key_ = key;
}

std::vector<std::uint16_t> DataViewWidget::onion_tracks(std::uint16_t active_track) const {
    std::vector<std::uint16_t> tracks{};

    editor::ProjectManager* manager = context.parent->controller().get_project_manager();
    if (manager == nullptr) {
        return tracks;
    }
    std::vector<midi::MIDITrack>* all_tracks = manager->get_tracks();
    if (all_tracks == nullptr) {
        return tracks;
    }

    // read the onion state from its owner every time; caching it here is what made the old strip
    // ignore the setting until the widget was rebuilt
    switch (context.parent->settings().pr_onion_state) {
    case VS_PianoRoll_OnionState::NoOnion:
        break;
    case VS_PianoRoll_OnionState::ViewPrevious:
        if (active_track > 0) {
            tracks.push_back(static_cast<std::uint16_t>(active_track - 1));
        }
        break;
    case VS_PianoRoll_OnionState::ViewNext:
        if (static_cast<std::size_t>(active_track) + 1 < all_tracks->size()) {
            tracks.push_back(static_cast<std::uint16_t>(active_track + 1));
        }
        break;
    case VS_PianoRoll_OnionState::ViewAll:
        for (std::size_t i = 0; i < all_tracks->size(); ++i) {
            if (i != active_track) {
                tracks.push_back(static_cast<std::uint16_t>(i));
            }
        }
        break;
    }

    return tracks;
}

std::uint64_t DataViewWidget::lane_key() const {
    if (data_editing_ == nullptr) {
        return 0;
    }

    const editor::DataEditSource* source = data_editing_->get_edit_source();
    const ViewSettings& settings = context.parent->settings();
    editor::ProjectManager* manager = context.parent->controller().get_project_manager();

    std::uint64_t track_revision = 0;
    if (manager != nullptr) {
        std::vector<midi::MIDITrack>* tracks = manager->get_tracks();
        const std::uint16_t track = data_editing_->lane_track();
        if (tracks != nullptr && track < tracks->size()) {
            track_revision = (*tracks)[track].revision;
        }
    }

    std::uint64_t key = static_cast<std::uint64_t>(source->event_source);
    key = (key << 8) | (source->channel & 0x0F);
    key = (key << 8) | (source->controller & 0xFF);
    key = (key << 16) | (data_editing_->lane_track() & 0xFFFF);
    key = (key << 4) | static_cast<std::uint64_t>(settings.pr_onion_state);
    key = (key << 4) | static_cast<std::uint64_t>(settings.pr_onion_coloring);
    key = (key << 16) | track_revision;
    return key;
}

editor::ViewRect DataViewWidget::gl_clip_rect() const {
    return context.work_rect;
}

void DataViewWidget::handle_input() {
    using namespace editor::data_edit_flags;

    // keep the projection in step with the source, track, onion state and project revision before
    // any gesture runs, so an edit always targets the lane that is actually being drawn
    refresh_lane();

    // the gl pass is scissored below the header, so the component must map the mouse against the
    // same rect or every hit would sit one header-height off
    if (component != nullptr) {
        component->set_rect(gl_clip_rect());
    }

    const bool pointer_in_panel = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if (!pointer_in_panel) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            data_editing_->disable_flag(DATA_EDIT_CLICKED_IN_RECT | DATA_EDIT_DRAW_EDIT_LINE);
        }
        return;
    }

    data_editing_->set_flag(DATA_EDIT_MOUSE_OVER_UI, context.parent->is_mouse_over_ui());
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

void DataViewWidget::draw_ui() {
    ImGui::PushID("data_viewer");

    auto* edit_source = data_editing_->get_edit_source();
    const auto& all_sources = EventSources::get_sources();
    auto* src_info = EventSources::get_source(edit_source->event_source);

    ImGui::Text("Events");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(100);
    if (ImGui::BeginCombo("##evt_source",
        EventSources::get_source(edit_source->event_source)->name.c_str())) {
        for (const auto& src : all_sources) {
            if (ImGui::Selectable(src.name.c_str(), edit_source->event_source == src.kind))
                edit_source->event_source = src.kind;
        }
        ImGui::EndCombo();
    }

    if (edit_source->event_source == editor::EventSourceKind::ControlChange) {
        ImGui::SameLine();
        ImGui::Text("Controller");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        if (ImGui::BeginCombo("##evt_controller",
            EventSources::cc_name_of(edit_source->controller).data())) {
            for (std::size_t controller = 0; controller < 128; controller++) {
                std::string name{ EventSources::cc_name_of(controller) };
                if (name.empty()) name = std::format("CC #{}", controller);
                if (ImGui::Selectable(name.c_str(), edit_source->controller == controller))
                    edit_source->controller = controller;
            }

            ImGui::EndCombo();
        }
    }

    if (edit_source->event_source == editor::EventSourceKind::ProgramChange) {
        ImGui::SameLine();
        ImGui::Text("Instrument/Patch");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        auto curr_patch = EventSources::pc_name_of(edit_source->patch);
        if (ImGui::BeginCombo("##evt_patch",
            curr_patch.data())) {
            for (std::size_t patch = 0; patch < 128; patch++) {
                if (ImGui::Selectable(EventSources::pc_name_of(patch).data(), edit_source->patch == patch))
                    edit_source->patch = patch;
            }
            ImGui::EndCombo();
        }

        ImGui::SameLine();
        if (ImGui::Button(std::format("Set to '{}' at playhead", curr_patch).c_str())) {
            data_editing_->set_value_at_current(edit_source->patch);
        }
    }

    // curve shape for the ramp tools, written straight into the editor's own tool state so the
    // editing pass and this header can never disagree about the active shape
    if (edit_source->event_source != editor::EventSourceKind::ProgramChange) {
        auto* tool = data_editing_->get_edit_tool();
        
        static constexpr const char* kToolNames[5] = {
            "Pencil", "Line", "Curve", "Selector", "Eraser"
        };
        const int tool_index = static_cast<int>(tool->tool_kind);

        ImGui::SameLine();
        
        ImGui::Text("Tool");
        ImGui::SameLine();
        ImGui::SetNextItemWidth(120);
        if (ImGui::BeginCombo("##evt_tool", kToolNames[tool_index])) {
            for (int i = 0; i < 5; i++) {
                if (ImGui::Selectable(kToolNames[i], tool_index == i))
                    tool->tool_kind = static_cast<editor::DataEditToolKind>(i);
            }
            ImGui::EndCombo();
        }

        if (tool->tool_kind == editor::DataEditToolKind::Curve) {
            static constexpr const char* kCurveNames[5] = {
                "Linear", "Exponential", "Logarithmic", "S-Curve", "Pulse"
            };
            const int curve_index = static_cast<int>(tool->curve_kind);

            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
            ImGui::SameLine();
            ImGui::Text("Curve");
            ImGui::SameLine();
            ImGui::SetNextItemWidth(120);
            if (ImGui::BeginCombo("##evt_curve", kCurveNames[curve_index])) {
                for (int i = 0; i < 5; ++i) {
                    if (ImGui::Selectable(kCurveNames[i], curve_index == i))
                        tool->curve_kind = static_cast<editor::DataEditCurveKind>(i);
                }
                ImGui::EndCombo();
            }

            ImGui::SameLine();
            ImGui::Text("Influence");
            ImGui::SameLine();

            ImGui::SetNextItemWidth(100);
            if (ImGui::SliderFloat("##tool_infl", &tool->influence, 1.0f, 5.0f, "%.1f")) {
                if (tool->influence < 1.0f) tool->influence = 1.0f;
                if (tool->influence > 5.0f) tool->influence = 5.0f;
            }

            ImGui::SameLine();
            ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(100);
        static double event_value = 0;
        ImGui::InputDouble("##event_set", &event_value);

        ImGui::SameLine();
        ImGui::SetNextItemWidth(10);
        if (ImGui::Button("Set")) {
            data_editing_->set_value_at_current(event_value);
        }
    }

    if (src_info->info.source_origin == editor::EventSourceOrigin::Channels) {
        ImGui::SameLine();
        ImGui::Text("All channels");
        ImGui::SameLine();
        bool all_channels = edit_source->channel == 0xFF;
        if (ImGui::Checkbox("##all_channels", &all_channels)) {
            if (all_channels)
                edit_source->channel = 0xFF;
            else
                edit_source->channel = data_editing_->controller()->get_toolbar_settings()->note_channel - 1;
        }
    }

    ImGui::PopID();
}

void DataViewWidget::draw_overlays() {
    data_editing_->draw_overlays();
    constexpr const std::size_t SEGMENTS = 20;
    constexpr const float SEGMENT_WIDTH = 1.0f / static_cast<float>(SEGMENTS);

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    if (data_editing_->get_flag(editor::data_edit_flags::DATA_EDIT_DRAW_EDIT_LINE)) {
        auto* tool = data_editing_->get_edit_tool();

        const auto [pt1, pt2] = data_editing_->get_data_view_line_points();
        
        if (tool->curve_kind == editor::DataEditCurveKind::Linear ||
            tool->tool_kind == editor::DataEditToolKind::Line)
            draw_list->AddLine({ pt1.x, pt1.y }, { pt2.x, pt2.y }, IM_COL32(255, 255, 255, 255), 1.0f);
        else {
            for (std::size_t s = 0; s < SEGMENTS; s++) {
                const float xf1 = static_cast<float>(s) * SEGMENT_WIDTH;
                const float xf2 = xf1 + SEGMENT_WIDTH;
                const float yf1 = data_editing_->curve_weight(tool->curve_kind, xf1, tool->influence);
                const float yf2 = data_editing_->curve_weight(tool->curve_kind, xf2, tool->influence);

                const float x1 = (1.0 - xf1) * pt1.x + xf1 * pt2.x;
                const float x2 = (1.0 - xf2) * pt1.x + xf2 * pt2.x;

                const float y1 = (1.0 - yf1) * pt1.y + yf1 * pt2.y;
                const float y2 = (1.0 - yf2) * pt1.y + yf2 * pt2.y;

                draw_list->AddLine({ x1, y1 }, { x2, y2 }, IM_COL32(255, 255, 255, 255), 1.5f);
            }
        }
    }

    draw_playhead_line();
}

}
