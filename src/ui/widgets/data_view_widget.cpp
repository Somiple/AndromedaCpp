#include "ui/widgets/data_view_widget.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <format>
#include <string>
#include <string_view>
#include <utility>

#include <imgui.h>

#include "ui/app/main_window.h"
#include "ui/rendering/data_view.h"
#include "editor/editing/data_editing/event_lane.h"
#include "core/util.h"

namespace {
using andromeda::editor::DataEditCurveKind;
using andromeda::editor::DataEditing;
using andromeda::editor::DataEditToolKind;
using andromeda::editor::EventSources;

#pragma region Vector icons
// Every icon is drawn from ImDrawList primitives on a unit square, so it stays crisp at any UI
// scale and any theme (it takes the text color), and needs no texture or font to be loaded.
enum class Icon {
    Select, Line, Curve, Pencil, Eraser,
    CurveLinear, CurveExponential, CurveLogarithmic, CurveSCurve, CurvePulse
};

ImVec2 lerp2(ImVec2 a, ImVec2 b, float t) {
    return { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t };
}

void dashed_line(ImDrawList* dl, ImVec2 a, ImVec2 b, ImU32 col, float thickness, float dash) {
    const float dx = b.x - a.x;
    const float dy = b.y - a.y;
    const float len = std::sqrt(dx * dx + dy * dy);
    if (len <= 0.0f) return;
    for (float d = 0.0f; d < len; d += dash * 2.0f) {
        const float e = std::min(d + dash, len);
        dl->AddLine(lerp2(a, b, d / len), lerp2(a, b, e / len), col, thickness);
    }
}

/// @param origin top-left of the square the icon is drawn in.
/// @param side side length of that square.
void draw_icon(ImDrawList* dl, Icon icon, ImVec2 origin, float side, ImU32 col) {
    const float pad = side * 0.22f;
    const float span = side - 2.0f * pad;
    const auto P = [&](float x, float y) { return ImVec2{ origin.x + pad + x * span, origin.y + pad + y * span }; };
    const float th = 1.6f;
    const float dot = 2.0f;

    // curve weight sampled across the unit square, y flipped so "up" means "more"
    const auto curve = [&](DataEditCurveKind kind, float exponent, bool end_dots) {
        constexpr int N = 24;
        ImVec2 pts[N];
        for (int i = 0; i < N; ++i) {
            const float t = static_cast<float>(i) / static_cast<float>(N - 1);
            const float w = static_cast<float>(DataEditing::curve_weight(kind, t, exponent));
            pts[i] = P(t, 1.0f - w);
        }
        dl->AddPolyline(pts, N, col, 0, th);
        if (end_dots) {
            dl->AddCircleFilled(pts[0], dot, col);
            dl->AddCircleFilled(pts[N - 1], dot, col);
        }
    };

    switch (icon) {
    case Icon::Select: {
        const float dash = 2.2f;
        dashed_line(dl, P(0, 0.1f), P(1, 0.1f), col, th, dash);
        dashed_line(dl, P(1, 0.1f), P(1, 0.9f), col, th, dash);
        dashed_line(dl, P(1, 0.9f), P(0, 0.9f), col, th, dash);
        dashed_line(dl, P(0, 0.9f), P(0, 0.1f), col, th, dash);
        break;
    }
    case Icon::Line:
    case Icon::CurveLinear: {
        dl->AddLine(P(0, 1), P(1, 0), col, th);
        if (icon == Icon::Line) {
            dl->AddCircleFilled(P(0, 1), dot, col);
            dl->AddCircleFilled(P(1, 0), dot, col);
        }
        break;
    }
    case Icon::Curve:             curve(DataEditCurveKind::SCurve, 2.5f, true); break;
    case Icon::CurveExponential:  curve(DataEditCurveKind::Exponential, 2.5f, false); break;
    case Icon::CurveLogarithmic:  curve(DataEditCurveKind::Logarithmic, 2.5f, false); break;
    case Icon::CurveSCurve:       curve(DataEditCurveKind::SCurve, 2.5f, false); break;
    case Icon::CurvePulse: {
        // the pulse weight is a hard step at exponent / (1 + exponent); drawn with a vertical edge
        const float edge = 2.0f / 3.0f;
        const ImVec2 pts[4] = { P(0, 1), P(edge, 1), P(edge, 0), P(1, 0) };
        dl->AddPolyline(pts, 4, col, 0, th);
        break;
    }
    case Icon::Pencil: {
        // a pencil pointing to the lower left: tip, shoulder, and a body running to the upper right
        const float d = 0.70710678f;
        const ImVec2 dir{ d, -d };
        const ImVec2 perp{ d, d };
        const auto at = [&](float along, float across) {
            return P(0.12f + dir.x * along + perp.x * across, 0.88f + dir.y * along + perp.y * across);
        };
        const ImVec2 body[5] = { at(0.0f, 0.0f), at(0.26f, 0.12f), at(0.98f, 0.12f), at(0.98f, -0.12f), at(0.26f, -0.12f) };
        dl->AddPolyline(body, 5, col, ImDrawFlags_Closed, th);
        dl->AddLine(at(0.26f, -0.12f), at(0.26f, 0.12f), col, th);
        break;
    }
    case Icon::Eraser: {
        // a tilted block with a band across it
        const ImVec2 a = P(0.10f, 0.62f);
        const ImVec2 b = P(0.48f, 0.12f);
        const ImVec2 c = P(0.92f, 0.46f);
        const ImVec2 e = P(0.54f, 0.96f);
        const ImVec2 quad[4] = { a, b, c, e };
        dl->AddPolyline(quad, 4, col, ImDrawFlags_Closed, th);
        dl->AddLine(lerp2(a, e, 0.45f), lerp2(b, c, 0.45f), col, th);
        break;
    }
    }
}

void hover_tip(const char* text) {
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", text);
}

/// A square toggle button showing a vector icon. Disabled buttons are dimmed and ignore clicks but
/// still explain themselves in the tooltip.
/// @return true when the button was clicked while enabled.
bool icon_toggle(const char* id, Icon icon, bool selected, bool enabled, const char* tooltip) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 size{ h + 6.0f, h };
    const ImVec2 pos = ImGui::GetCursorScreenPos();

    const bool pressed = ImGui::InvisibleButton(id, size);
    const bool hovered = ImGui::IsItemHovered();

    ImDrawList* dl = ImGui::GetWindowDrawList();
    if (selected) {
        dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y },
                          ImGui::GetColorU32(ImGuiCol_ButtonActive), 4.0f);
    } else if (hovered && enabled) {
        dl->AddRectFilled(pos, { pos.x + size.x, pos.y + size.y },
                          ImGui::GetColorU32(ImGuiCol_ButtonHovered), 4.0f);
    }

    draw_icon(dl, icon, { pos.x + 3.0f, pos.y }, h,
              ImGui::GetColorU32(enabled ? ImGuiCol_Text : ImGuiCol_TextDisabled));

    if (hovered) ImGui::SetTooltip("%s", tooltip);
    return pressed && enabled;
}
#pragma endregion

void vsep() {
    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();
}

std::string lower(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return s;
}

/// A combo with a search box, for the 128-entry controller and instrument lists.
/// @param name_of maps an index to its name; an empty name shows as "#index".
/// @return true when the value changed.
template <typename NameFn>
bool searchable_picker(const char* id, std::uint8_t& value, float width, std::size_t count, NameFn&& name_of) {
    const auto label_of = [&](std::size_t v) {
        const std::string name{ name_of(v) };
        return name.empty() ? std::format("#{}", v) : std::format("{}  {}", v, name);
    };

    // one buffer per instantiation (each caller passes its own lambda), cleared every time it opens
    static char search[32] = "";
    bool changed = false;

    const std::string preview = label_of(value);
    ImGui::SetNextItemWidth(width);
    if (ImGui::BeginCombo(id, preview.c_str())) {
        if (ImGui::IsWindowAppearing()) {
            search[0] = '\0';
            ImGui::SetKeyboardFocusHere();
        }
        ImGui::SetNextItemWidth(-FLT_MIN);
        ImGui::InputTextWithHint("##search", "Search...", search, sizeof(search));
        const std::string needle = lower(search);

        for (std::size_t v = 0; v < count; ++v) {
            const std::string label = label_of(v);
            if (!needle.empty() && lower(label).find(needle) == std::string::npos) continue;

            const bool is_current = v == value;
            if (ImGui::Selectable(label.c_str(), is_current)) {
                value = static_cast<std::uint8_t>(v);
                changed = true;
            }
            if (is_current) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    return changed;
}

/// Snap grid presets. The value is grid divisions per quarter note (snap interval = PPQ / value).
struct SnapPreset { int divisions; const char* label; };
constexpr SnapPreset kSnapPresets[] = {
    { 0, "Off" }, { 1, "1/4" }, { 2, "1/8" }, { 4, "1/16" }, { 8, "1/32" }, { 16, "1/64" }, { 32, "1/128"},
    { 3, "1/8 triplet" }, { 6, "1/16 triplet" }, { 12, "1/32 triplet" },
};

std::string snap_label(int divisions) {
    for (const SnapPreset& preset : kSnapPresets) {
        if (preset.divisions == divisions) return preset.label;
    }
    return std::format("{} per beat", divisions);
}
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

    const bool gesture_active = data_editing_->get_flag(DATA_EDIT_CLICKED_IN_RECT) ||
                                data_editing_->get_flag(DATA_EDIT_DRAW_EDIT_LINE);

    // a drag that started in the lane keeps going when the pointer leaves the panel (the editor
    // clamps the pointer to the lane), so the release still commits it; before, leaving the panel
    // mid-drag dropped the release and left the selection box stuck on screen
    const bool pointer_in_panel = ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);
    if (!pointer_in_panel && !gesture_active) {
        return;
    }

    data_editing_->set_flag(DATA_EDIT_MOUSE_OVER_UI, context.parent->is_mouse_over_ui());
    data_editing_->set_flag(DATA_EDIT_ANY_DIALOG_OPEN, false);

    data_editing_->update();

    // no new gesture against a lane that is mid-rebuild: it would edit stale or missing points
    if (pointer_in_panel && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !lane_building_) {
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
    // copy / cut / paste need the command key: a bare C or V press used to copy and paste
    const bool command = io.KeyCtrl || io.KeySuper;
    keys.del = keys_free && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) ||
                             ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
    keys.copy = keys_free && command && ImGui::IsKeyPressed(ImGuiKey_C, false);
    keys.cut = keys_free && command && ImGui::IsKeyPressed(ImGuiKey_X, false);
    keys.paste = keys_free && command && ImGui::IsKeyPressed(ImGuiKey_V, false);

    data_editing_->on_key_down(keys);
}

void DataViewWidget::draw_ui() {
    ImGui::PushID("data_viewer");

    auto* source = data_editing_->get_edit_source();
    auto* tool = data_editing_->get_edit_tool();
    const bool is_program = source->event_source == editor::EventSourceKind::ProgramChange;

    // keep the tool valid for the lane: instrument changes are discrete, so only the selector
    // applies, and the pencil / eraser are not implemented yet so they must never stay active
    const bool kind_is_ramp = tool->tool_kind == DataEditToolKind::Line ||
                              tool->tool_kind == DataEditToolKind::Curve;
    if (tool->tool_kind == DataEditToolKind::Pencil || (is_program && kind_is_ramp)) {
        tool->tool_kind = is_program ? DataEditToolKind::Selector : DataEditToolKind::Line;
    }

    draw_tool_buttons(is_program);
    vsep();
    draw_source_controls();
    draw_ramp_options();
    draw_right_cluster(is_program);

    ImGui::PopID();
}

void DataViewWidget::draw_tool_buttons(bool is_program) {
    auto* tool = data_editing_->get_edit_tool();

    struct ToolButton {
        DataEditToolKind kind;
        Icon icon;
        const char* id;
        const char* tip;
        bool implemented;
        bool ramp;  // draws values between two points, which instrument changes cannot do
    };

    static constexpr ToolButton kTools[] = {
        { DataEditToolKind::Pencil, Icon::Pencil, "##tool_pencil",
          "Pencil\nNot implemented yet.", false, true },
        { DataEditToolKind::Eraser, Icon::Eraser, "##tool_eraser",
          "Eraser", true, false },
        { DataEditToolKind::Selector, Icon::Select, "##tool_select",
          "Select\nDrag a box over events. Shift / Alt change whether it adds or removes.", true, false },
        { DataEditToolKind::Line, Icon::Line, "##tool_line",
          "Line\nDrag to draw a straight ramp of values.", true, true },
        { DataEditToolKind::Curve, Icon::Curve, "##tool_curve",
          "Curve\nDrag to draw a shaped ramp; pick the shape next to it.", true, true },
    };

    bool first = true;
    for (const ToolButton& button : kTools) {
        if (!first) ImGui::SameLine(0.0f, 2.0f);
        first = false;

        const bool enabled = button.implemented && !(is_program && button.ramp);
        const char* tip = button.tip;
        std::string disabled_tip;
        if (button.implemented && !enabled) {
            disabled_tip = std::string(button.tip) + "\nNot available for instrument changes.";
            tip = disabled_tip.c_str();
        }

        if (icon_toggle(button.id, button.icon, tool->tool_kind == button.kind, enabled, tip)) {
            tool->tool_kind = button.kind;
            // a half-drawn gesture of the old tool must not finish under the new one
            data_editing_->cancel_gesture();
        }
    }
}

void DataViewWidget::draw_source_controls() {
    auto* source = data_editing_->get_edit_source();
    const auto& all_sources = EventSources::get_sources();

    ImGui::SetNextItemWidth(118.0f);
    if (ImGui::BeginCombo("##evt_source", EventSources::get_source(source->event_source)->name.c_str())) {
        for (const auto& src : all_sources) {
            if (ImGui::Selectable(src.name.c_str(), source->event_source == src.kind) &&
                source->event_source != src.kind) {
                source->event_source = src.kind;
                // selection boxes and half-drawn gestures belong to the lane they were made in
                data_editing_->cleanup();
            }
        }
        ImGui::EndCombo();
    }
    hover_tip("Which events this lane shows and edits");

    const auto* src_info = EventSources::get_source(source->event_source);

    if (source->event_source == editor::EventSourceKind::ControlChange) {
        ImGui::SameLine();
        searchable_picker("##evt_controller", source->controller, 170.0f, 128,
            [](std::size_t v) { return EventSources::cc_name_of(v); });
        hover_tip("Controller number");
    }

    if (source->event_source == editor::EventSourceKind::ProgramChange) {
        ImGui::SameLine();
        searchable_picker("##evt_patch", source->patch, 200.0f, 128,
            [](std::size_t v) { return EventSources::pc_name_of(v); });
        hover_tip("Instrument to insert");
    }

    if (source->event_source == editor::EventSourceKind::PolyPressure) {
        ImGui::SameLine();
        searchable_picker("##evt_key", source->key, 170.0f, 128,
            [](std::size_t v) { return andromeda::util::get_key_name(v); });
        hover_tip("Key to change pressure");
    }

    if (src_info->info.source_origin == editor::EventSourceOrigin::Channels) {
        ImGui::SameLine();
        const std::string preview = source->channel == 0xFF
            ? std::string("All ch")
            : std::format("Ch {}", source->channel + 1);
        ImGui::SetNextItemWidth(72.0f);
        if (ImGui::BeginCombo("##evt_channel", preview.c_str())) {
            if (ImGui::Selectable("All channels", source->channel == 0xFF)) {
                source->channel = 0xFF;
            }
            ImGui::Separator();
            for (std::uint8_t channel = 0; channel < 16; channel++) {
                const std::string label = std::format("Channel {}", channel + 1);
                if (ImGui::Selectable(label.c_str(), source->channel == channel)) {
                    source->channel = channel;
                }
            }
            ImGui::EndCombo();
        }
        hover_tip("Only show and edit events on this channel");
    }
}

void DataViewWidget::draw_ramp_options() {
    auto* tool = data_editing_->get_edit_tool();

    // snapping and curve shape only mean something to the ramp tools, so they are not shown
    // (and do not push everything else around) while the selector is active
    if (tool->tool_kind != DataEditToolKind::Line && tool->tool_kind != DataEditToolKind::Curve) {
        return;
    }

    vsep();

    const std::string snap_preview = "Snap " + snap_label(static_cast<int>(tool->resolution));
    ImGui::SetNextItemWidth(104.0f);
    if (ImGui::BeginCombo("##snap", snap_preview.c_str())) {
        for (const SnapPreset& preset : kSnapPresets) {
            if (ImGui::Selectable(preset.label, static_cast<int>(tool->resolution) == preset.divisions)) {
                tool->resolution = static_cast<editor::MIDITick>(preset.divisions);
            }
        }
        ImGui::EndCombo();
    }
    hover_tip("Grid the ramp's endpoints and steps snap to");

    if (tool->tool_kind != DataEditToolKind::Curve) return;

    struct CurveButton {
        DataEditCurveKind kind;
        Icon icon;
        const char* id;
        const char* tip;
    };
    static constexpr CurveButton kCurves[] = {
        { DataEditCurveKind::Linear, Icon::CurveLinear, "##curve_linear", "Linear" },
        { DataEditCurveKind::Exponential, Icon::CurveExponential, "##curve_exp", "Exponential\nSlow start, fast finish." },
        { DataEditCurveKind::Logarithmic, Icon::CurveLogarithmic, "##curve_log", "Logarithmic\nFast start, slow finish." },
        { DataEditCurveKind::SCurve, Icon::CurveSCurve, "##curve_s", "S-curve\nSlow at both ends." },
        { DataEditCurveKind::Pulse, Icon::CurvePulse, "##curve_pulse", "Pulse\nHolds the start value, then jumps." },
    };

    vsep();
    bool first = true;
    for (const CurveButton& curve : kCurves) {
        if (!first) ImGui::SameLine(0.0f, 2.0f);
        first = false;
        if (icon_toggle(curve.id, curve.icon, tool->curve_kind == curve.kind, true, curve.tip)) {
            tool->curve_kind = curve.kind;
        }
    }

    // a linear ramp has nothing to bend
    if (tool->curve_kind != DataEditCurveKind::Linear) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(76.0f);
        if (ImGui::SliderFloat("##tool_infl", &tool->influence, 1.0f, 5.0f, "Bend %.1f")) {
            tool->influence = std::clamp(tool->influence, 1.0f, 5.0f);
        }
        hover_tip("How strongly the curve bends");
    }
}

void DataViewWidget::draw_right_cluster(bool is_program) {
    auto* source = data_editing_->get_edit_source();
    const ImGuiStyle& style = ImGui::GetStyle();
    const float spacing = style.ItemSpacing.x;

    // the box starts at the lane's default value, and again whenever the lane changes
    const int key = static_cast<int>(source->event_source) * 256 + source->controller;
    if (key != set_value_key_) {
        set_value_key_ = key;
        set_value_ = data_editing_->get_source_default();
    }

    const char* set_label = is_program ? "Set at playhead" : "Set";
    const float button_w = ImGui::CalcTextSize(set_label).x + style.FramePadding.x * 2.0f;
    const float input_w = 72.0f;
    const float set_w = is_program ? button_w : input_w + spacing + button_w;

    const auto& stats = data_editing_->get_stats();
    std::string stats_text;
    float stats_w = 0.0f;
    if (stats.stats_present) {
        stats_text = std::format("Pointer {:.1f}   Lane {:.1f}", stats.value_at_mouse, stats.present_value_at_mouse);
        // the text, plus the separator and its spacing
        stats_w = ImGui::CalcTextSize(stats_text.c_str()).x + spacing * 3.0f;
    }

    ImGui::SameLine();
    const float avail = ImGui::GetContentRegionAvail().x;
    if (avail < set_w) return;  // no room: the lane matters more than the shortcut

    const bool show_stats = stats_w > 0.0f && avail >= set_w + stats_w;
    const float used = show_stats ? set_w + stats_w : set_w;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + avail - used);

    if (show_stats) {
        ImGui::TextUnformatted(stats_text.c_str());
        hover_tip("Pointer: the value under the mouse.\nLane: the value the lane holds at the mouse's position.");
        vsep();
    }

    if (is_program) {
        if (ImGui::Button(set_label)) {
            data_editing_->set_value_at_current(source->patch);
        }
        hover_tip(std::format("Insert '{}' at the playhead", EventSources::pc_name_of(source->patch)).c_str());
        return;
    }

    ImGui::SetNextItemWidth(input_w);
    const bool entered = ImGui::InputDouble("##event_set", &set_value_, 0.0, 0.0, "%.3f",
                                            ImGuiInputTextFlags_EnterReturnsTrue);
    hover_tip("Value to write at the playhead");
    ImGui::SameLine();
    const bool clicked = ImGui::Button(set_label);
    hover_tip("Write the value at the playhead");
    if (entered || clicked) {
        data_editing_->set_value_at_current(set_value_);
    }
}

void DataViewWidget::draw_overlays() {
    data_editing_->draw_overlays();
    constexpr const std::size_t SEGMENTS = 64;  // enough that the pulse's jump reads as a step
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