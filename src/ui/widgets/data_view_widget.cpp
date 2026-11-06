#include "ui/widgets/data_view_widget.h"

#include <chrono>
#include <cstdio>
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
    // created_renderer->use_note_cache(std::move(note_cache));

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

    // per-frame slice of the streamed rebuild; small enough to keep the UI at frame rate
    constexpr auto LANE_BUDGET = std::chrono::microseconds(2000);

    const LaneKey key = lane_key();

    // what the front already shows is current: drop any build that was chasing an older state
    if (lane_front_valid_ && key == lane_key_) {
        if (lane_building_) {
            event_lane_->cancel_rebuild();
            lane_building_ = false;
        }
        return;
    }

    // an edit (or onion/track change) invalidates a build in flight, so restart it. Doing this
    // before pump() also guarantees the builder never reads notes edited after its snapshot.
    if (!lane_building_ || key != requested_key_) {
        // only the revisions moved: the old points are a good stand-in until the new ones commit.
        // a different source/track/channel/onion must not be shown as the new lane, so clear it.
        const bool same_shape = lane_front_valid_ &&
            key.lane_track == lane_key_.lane_track &&
            key.event_source == lane_key_.event_source &&
            key.source_info_key == lane_key_.source_info_key &&
            key.onion_key == lane_key_.onion_key;

        event_lane_->request_rebuild(onion_tracks(data_editing_->lane_track()), same_shape);
        if (!same_shape) lane_front_valid_ = false;
        requested_key_ = key;
        lane_building_ = true;
    }

    if (event_lane_->pump(LANE_BUDGET)) {
        return; // still streaming
    }

    lane_key_ = requested_key_;
    lane_front_valid_ = true;
    lane_building_ = false;
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

DataViewWidget::LaneKey DataViewWidget::lane_key() const {
    if (data_editing_ == nullptr) return {};

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

    LaneKey lane_key{};
    lane_key.track_revision = track_revision;
    lane_key.data_revision = data_editing_->revision();
    lane_key.lane_track = data_editing_->lane_track();
    lane_key.event_source = static_cast<uint8_t>(source->event_source);
    lane_key.source_info_key = (static_cast<uint16_t>(source->channel) << 8) |
                                static_cast<uint16_t>(source->controller);
    lane_key.onion_key = (static_cast<uint8_t>(settings.pr_onion_state) << 4) |
                         (static_cast<uint8_t>(settings.pr_onion_coloring));
    return lane_key;
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

    // no new gesture against a lane that is mid-rebuild: it would edit stale or missing points
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !lane_building_) {
        data_editing_->on_mouse_down();
    }
    if (ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
        data_editing_->on_mouse_move();
    }
    if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
        data_editing_->on_mouse_up();
    }

    const ImGuiIO& io = ImGui::GetIO();
    editor::DataEditing::KeyState keys;
    const bool keys_free = !io.WantCaptureKeyboard;
    keys.del = keys_free && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
    keys.copy = keys_free && ImGui::IsKeyPressed(ImGuiKey_C, false);
    keys.paste = keys_free && ImGui::IsKeyPressed(ImGuiKey_V, false);

    data_editing_->on_key_down(keys);
}

void DataViewWidget::draw_ui() {
    ImGui::PushID("data_viewer");

    auto* edit_source = data_editing_->get_edit_source();
    const auto& all_sources = EventSources::get_sources();
    auto* src_info = EventSources::get_source(edit_source->event_source);

#pragma region Event Source
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

	// helper lambda for sources that have a "second parameter" (channel, controller, patch) to edit (e.g. CC number, program number, etc.)
    const auto& src_dropdown_with_search =
        [&](const char* id, const char* label, auto& value,
            const auto& name_of_func, std::size_t max_value, const auto& placeholder_name_func) -> void {
        ImGui::Text(label);
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);

		static char search_buffer_[32] = "";

        // moved this lower search here 
        std::string lower_search = search_buffer_;
        std::transform(lower_search.begin(), lower_search.end(), lower_search.begin(), ::tolower);

        if (ImGui::BeginCombo(id, name_of_func(value))) {
            ImGui::InputText(std::format("{}_search", id).c_str(), search_buffer_, IM_ARRAYSIZE(search_buffer_));

            for (std::size_t val = 0; val < max_value; val++) {
                std::string name{ name_of_func(val) };
                if (name.empty()) name = placeholder_name_func(val);

                if (strlen(search_buffer_) > 0) {
                    std::string lower_name = name;
                    std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                    
                    if (lower_name.find(lower_search) == std::string::npos) {
                        continue;
                    }
                }

                if (ImGui::Selectable(name.c_str(), value == val)) value = val;
            }

            ImGui::EndCombo();
        }
    };

    if (edit_source->event_source == editor::EventSourceKind::ControlChange) {
        ImGui::SameLine();
        src_dropdown_with_search("##evt_controller", "Controller", edit_source->controller,
            [&](const auto& val) {
                return EventSources::cc_name_of(val).data();
            }, 128,
            [&](const auto& val) { return std::format("CC #{}", val); }
        );
    }

    if (edit_source->event_source == editor::EventSourceKind::ProgramChange) {
        ImGui::SameLine();
        auto curr_patch = EventSources::pc_name_of(edit_source->patch);

        src_dropdown_with_search("##evt_patch", "Instrument/Patch", edit_source->patch,
            [&](const auto& val) {
                return EventSources::pc_name_of(val).data();
            }, 128,
            [&](const auto& val) { return std::format("Instrument #{}", val); }
        );

        ImGui::SameLine();
        if (ImGui::Button(std::format("Set to '{}' at playhead", curr_patch).c_str())) {
            data_editing_->set_value_at_current(edit_source->patch);
        }
    }
#pragma endregion

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    // curve shape for the ramp tools, written straight into the editor's own tool state so the
    // editing pass and this header can never disagree about the active shape
    if (edit_source->event_source != editor::EventSourceKind::ProgramChange) {
        auto* tool = data_editing_->get_edit_tool();
        
        // TODO: icons instead
        static constexpr const char* kToolNames[5] = {
            "Pencil", "Line", "Curve", "Selector", "Eraser"
        };
        
        const int tool_index = static_cast<int>(tool->tool_kind);
        for (int i = 0; i < 5; i++) {
            if (i > 0) { ImGui::Spacing(); ImGui::SameLine(); }
            if (ImGui::Selectable(kToolNames[i], tool_index == i, 0, ImVec2{40, 0})) {
                tool->tool_kind = static_cast<editor::DataEditToolKind>(i);
			}
            ImGui::SameLine();
        }
        
        ImGui::Spacing();

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
        }

        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        ImGui::Text("Resolution");
        ImGui::SameLine();
		int res = static_cast<int>(tool->resolution);
        ImGui::SetNextItemWidth(100);
		if (ImGui::InputInt("##resolution", &res, 0, 0)) tool->resolution = static_cast<editor::MIDITick>(std::max(0, res));
        
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        ImGui::SetNextItemWidth(10);
        static double event_value = 0;
        if (ImGui::Button("Set")) {
            data_editing_->set_value_at_current(event_value);
        }
        ImGui::SameLine();
        ImGui::SetNextItemWidth(100);
        ImGui::InputDouble("##event_set", &event_value, 0.0, 0.0, "%.3f");
    }

    if (src_info->info.source_origin == editor::EventSourceOrigin::Channels) {
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();
        ImGui::Text("Channel");
        ImGui::SameLine();
		ImGui::SetNextItemWidth(80);
        if (ImGui::BeginCombo("##evt_channel", edit_source->channel == 0xFF ? "All" : std::to_string(edit_source->channel + 1).c_str())) {
            if (ImGui::Selectable("All", edit_source->channel == 0xFF)) {
                edit_source->channel = 0xFF;
            }
            ImGui::Separator();
            for (std::uint8_t channel = 0; channel < 16; channel++) {
                if (ImGui::Selectable(std::to_string(channel + 1).c_str(), edit_source->channel == channel)) {
                    edit_source->channel = channel;
                }
            }
            ImGui::EndCombo();
        };
    }

    const auto& stats = data_editing_->get_stats();
    if (stats.stats_present) {
        ImGui::SameLine();
        ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
        ImGui::SameLine();

        ImGui::Text("v: %.1f | c: %.1f", stats.value_at_mouse, stats.present_value_at_mouse);
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

    draw_lane_overlays();
    draw_build_progress();
    draw_playhead_line();
}

void DataViewWidget::refresh_pc_labels() {
    const auto* edit_source = data_editing_->get_edit_source();
    editor::ProjectManager* manager = data_editing_->controller()->get_project_manager();
    std::vector<midi::MIDITrack>* tracks = manager ? manager->get_tracks() : nullptr;
    const std::uint16_t track = data_editing_->lane_track();

    if (tracks == nullptr || track >= tracks->size()) {
        pc_labels_.clear();
        pc_labels_valid_ = false;
        return;
    }

    const auto& midi_track = (*tracks)[track];
    const PcLabelKey key{ track, edit_source->channel, data_editing_->revision() };

    if (pc_labels_valid_ && key == pc_key_) {
        return;
    }

    pc_labels_.clear();
    for (const auto& event : midi_track.get_channel_evs()) {
        if (!std::holds_alternative<midi::ProgramChange>(event.event_type)) continue;
        if (edit_source->channel != 0xFF && edit_source->channel != event.channel) continue;

        const auto& pc = std::get<midi::ProgramChange>(event.event_type);

        // several program changes on one tick collapse into a single label
        if (!pc_labels_.empty() && pc_labels_.back().tick == event.tick) {
            auto& last = pc_labels_.back();
            if (last.program.has_value() && *last.program != pc.program) {
                last.program.reset();
            }
            continue;
        }

        pc_labels_.push_back({ event.tick, event.channel, pc.program });
    }

    pc_key_ = key;
    pc_labels_valid_ = true;
}

void DataViewWidget::draw_build_progress() {
    if (!lane_building_ || event_lane_ == nullptr) return;

    char label[48];
    std::snprintf(label, sizeof(label), "Building lane... %d%%",
                  static_cast<int>(event_lane_->progress() * 100.0f));

    const auto& rect = context.work_rect;
    ImGui::GetWindowDrawList()->AddText({ rect.left + 8.0f, rect.top + 6.0f },
                                        IM_COL32(220, 220, 220, 255), label);
}

void DataViewWidget::draw_lane_overlays() {
    if (data_editing_->get_edit_source()->event_source != editor::EventSourceKind::ProgramChange) {
        return;
    }

    refresh_pc_labels();
    if (pc_labels_.empty()) return;

    const auto& colors = context.parent->colors().get_colors();
    const auto& central_view = context.parent->get_central_tick_view();
    const auto& rect = context.work_rect;
    ImDrawList* draw_list = ImGui::GetWindowDrawList();

    const auto view_start = static_cast<midi::MIDITick>(central_view.pos);
    const auto view_end = static_cast<midi::MIDITick>(central_view.pos + central_view.zoom);

    // labels are sorted by tick, so only the visible slice is visited
    auto it = std::lower_bound(pc_labels_.begin(), pc_labels_.end(), view_start,
        [](const ProgramChangeLabel& label, midi::MIDITick tick) { return label.tick < tick; });

    for (; it != pc_labels_.end() && it->tick <= view_end; ++it) {
        const float pos_norm = static_cast<float>(it->tick - central_view.pos) / central_view.zoom;
        const float real_pos = pos_norm * rect.width + rect.left;

        const auto& c = colors[it->channel];
        const ImU32 color = it->program.has_value()
            ? static_cast<ImU32>(ImColor(c[0], c[1], c[2]))
            : IM_COL32_WHITE;

        draw_list->AddLine({ real_pos, rect.top }, { real_pos, rect.top + rect.height }, color);

        const std::string_view name = it->program.has_value()
            ? EventSources::pc_name_of(*it->program)
            : std::string_view{ "Multiple instruments" };

        draw_list->AddText({ real_pos + 3.0f, rect.top + rect.height * 0.5f }, color,
            name.data(), name.data() + name.size());
    }
}

}