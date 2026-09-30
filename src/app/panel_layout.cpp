#include "app/panel_layout.h"

#include <imgui_internal.h>

#include <algorithm>

namespace andromeda::app {

namespace {

constexpr ImGuiWindowFlags PANEL_FLAGS = ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
                                         ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoSavedSettings |
                                         ImGuiWindowFlags_NoBringToFrontOnFocus |
                                         ImGuiWindowFlags_NoNavFocus;

}

void PanelLayout::begin_frame(ImVec2 pos, ImVec2 size) {
    rect_min_ = pos;
    rect_max_ = {pos.x + size.x, pos.y + size.y};
    pointer_over_panel_ = false;
    separator_enabled_ = true;
}

bool PanelLayout::top(const char* id, float default_extent, bool auto_size,
                      ImGuiWindowFlags extra_flags) {
    return begin_panel(id, Side::Top, default_extent, auto_size, extra_flags);
}

bool PanelLayout::bottom(const char* id, float default_extent, bool auto_size,
                         ImGuiWindowFlags extra_flags) {
    return begin_panel(id, Side::Bottom, default_extent, auto_size, extra_flags);
}

bool PanelLayout::left(const char* id, float default_extent, bool auto_size,
                       ImGuiWindowFlags extra_flags) {
    return begin_panel(id, Side::Left, default_extent, auto_size, extra_flags);
}

bool PanelLayout::right(const char* id, float default_extent, bool auto_size,
                        ImGuiWindowFlags extra_flags) {
    return begin_panel(id, Side::Right, default_extent, auto_size, extra_flags);
}

bool PanelLayout::begin_panel(const char* id, Side side, float default_extent, bool auto_size,
                              ImGuiWindowFlags extra_flags) {
    PanelState& state = states_[id];
    state.min_extent = default_extent;
    if (!state.measured) {
        state.extent = default_extent;
    }

    const float avail_w = rect_max_.x - rect_min_.x;
    const float avail_h = rect_max_.y - rect_min_.y;

    const float max_v = std::max(0.0f, avail_h - 32.0f);
    const float max_h = std::max(0.0f, avail_w - 32.0f);

    ImVec2 pos{};
    ImVec2 size{};
    float extent = state.extent;

    switch (side) {
    case Side::Top:
        extent = std::clamp(extent, 0.0f, max_v);
        pos = rect_min_;
        size = {avail_w, extent};
        rect_min_.y += extent;
        break;
    case Side::Bottom:
        extent = std::clamp(extent, 0.0f, max_v);
        pos = {rect_min_.x, rect_max_.y - extent};
        size = {avail_w, extent};
        rect_max_.y -= extent;
        break;
    case Side::Left:
        extent = std::clamp(extent, 0.0f, max_h);
        pos = rect_min_;
        size = {extent, avail_h};
        rect_min_.x += extent;
        break;
    case Side::Right:
        extent = std::clamp(extent, 0.0f, max_h);
        pos = {rect_max_.x - extent, rect_min_.y};
        size = {extent, avail_h};
        rect_max_.x -= extent;
        break;
    }

    ImGui::SetNextWindowPos(pos);
    ImGui::SetNextWindowSize(size);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);

    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);

    // must stay zero and be pushed before begin, where imgui clamps to a min size
    ImGui::PushStyleVar(ImGuiStyleVar_WindowMinSize, ImVec2(0.0f, 0.0f));

    open_center_offset_ = 0.0f;
    open_id_ = id;
    open_side_ = side;
    open_auto_size_ = auto_size;
    open_ = ImGui::Begin(id, nullptr, PANEL_FLAGS | extra_flags);

    if (open_) {
        if (!auto_size && state.content_measured) {
            const float pad = ImGui::GetStyle().WindowPadding.y;
            const float inner = (side == Side::Left || side == Side::Right)
                                    ? size.x - ImGui::GetStyle().WindowPadding.x * 2.0f
                                    : size.y - pad * 2.0f;

            const float slack = inner - state.content;
            if (slack > 0.0f && side != Side::Left && side != Side::Right) {
                open_center_offset_ = slack * 0.5f;
                ImGui::SetCursorPosY(ImGui::GetCursorPosY() + open_center_offset_);
            }
        }

        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)) {
            pointer_over_panel_ = true;
        }
    }

    return open_;
}

void PanelLayout::end_panel() {
    PanelState& state = states_[open_id_];

    if (open_) {
        const ImGuiWindow* w = ImGui::GetCurrentWindow();
        float content = 0.0f;
        if (open_side_ == Side::Left || open_side_ == Side::Right) {
            content = w->DC.CursorMaxPos.x - w->DC.CursorStartPos.x;
        } else {
            content = w->DC.CursorMaxPos.y - w->DC.CursorStartPos.y;

            if (w->DC.CurrLineSize.y > 0.0f) {
                const float in_progress =
                    (w->DC.CursorPos.y - w->DC.CursorStartPos.y) + w->DC.CurrLineSize.y;
                content = std::max(content, in_progress);
            }

            content -= open_center_offset_;
        }
        if (content > 0.0f) {
            state.content = content;
            state.content_measured = true;
        }
    }

    if (open_ && state.content_measured) {
        const ImGuiStyle& style = ImGui::GetStyle();
        const float pad = (open_side_ == Side::Left || open_side_ == Side::Right)
                              ? style.WindowPadding.x
                              : style.WindowPadding.y;

        const float needed = state.content + pad * 2.0f;
        const float extent = open_auto_size_ ? std::max(needed, state.min_extent)
                                             : std::max(state.min_extent, needed);

        if (extent > 0.0f) {
            state.extent = extent;
            state.measured = true;
        }
    }

    if (open_ && separator_enabled_) {
        ImDrawList* draw = ImGui::GetWindowDrawList();
        const ImVec2 p0 = ImGui::GetWindowPos();
        const ImVec2 sz = ImGui::GetWindowSize();
        const ImU32 col = ImGui::GetColorU32(ImGuiCol_Separator);

        draw->PushClipRect(p0, {p0.x + sz.x, p0.y + sz.y}, false);

        switch (open_side_) {
        case Side::Top:
            draw->AddLine({p0.x, p0.y + sz.y - 1.0f}, {p0.x + sz.x, p0.y + sz.y - 1.0f}, col);
            break;
        case Side::Bottom:
            draw->AddLine({p0.x, p0.y}, {p0.x + sz.x, p0.y}, col);
            break;
        case Side::Left:
            draw->AddLine({p0.x + sz.x - 1.0f, p0.y}, {p0.x + sz.x - 1.0f, p0.y + sz.y}, col);
            break;
        case Side::Right:
            draw->AddLine({p0.x, p0.y}, {p0.x, p0.y + sz.y}, col);
            break;
        }

        draw->PopClipRect();
    }

    ImGui::End();
    ImGui::PopStyleVar(3);
    open_ = false;
}

}
