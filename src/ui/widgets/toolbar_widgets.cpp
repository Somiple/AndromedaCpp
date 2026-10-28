#include "ui/widgets/toolbar_widgets.h"

#include <algorithm>
#include <cstdio>

#include <imgui_internal.h>

namespace andromeda::app::toolbar {

void center_row_against(float row_height) {
    ImGuiWindow* w = ImGui::GetCurrentWindow();

    const float half = std::max(0.0f, (row_height - ImGui::GetFrameHeight()) * 0.5f);
    w->DC.CurrLineTextBaseOffset = half + ImGui::GetStyle().FramePadding.y;
}

float toolbar_row_height() {
    return TOOLBAR_ICON_SIZE + ImGui::GetStyle().FramePadding.y * 2.0f;
}

void toolbar_separator() {
    ImGui::SameLine();
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const float h = ImGui::GetFrameHeight();
    ImGui::GetWindowDrawList()->AddLine({p.x + 3.0f, p.y}, {p.x + 3.0f, p.y + h},
                                        ImGui::GetColorU32(ImGuiCol_Separator));
    ImGui::Dummy(ImVec2(7.0f, h));
    ImGui::SameLine();
}

bool icon_button(const char* id, const ImageHandle& img, bool selected, bool enabled,
                 const char* fallback_label, float size, bool frame) {
    bool clicked = false;

    if (size <= 0.0f) {
        size = img.valid() ? img.width : 20.0f;
    }

    if (!frame) {
        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1, 1, 1, 0.08f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive, ImVec4(1, 1, 1, 0.16f));
        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
    }

    if (img.valid()) {
        const ImVec4 tint = enabled ? ImVec4(1.0f, 1.0f, 1.0f, 1.0f)
                                    : ImVec4(1.0f, 1.0f, 1.0f, 0.45f);

        if (selected) {
            ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.7216f, 0.5686f, 0.8627f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8216f, 0.6686f, 0.9627f, 1.0f));
        }

        clicked = ImGui::ImageButton(id, static_cast<ImTextureID>(img.texture_id),
                                     ImVec2(size, size), ImVec2(0, 0), ImVec2(1, 1),
                                     ImVec4(0, 0, 0, 0), tint);

        if (selected) {
            ImGui::PopStyleColor(2);
        }

        if (!enabled) {
            clicked = false;
        }
    } else {
        ImGui::BeginDisabled(!enabled);
        clicked = ImGui::Button(fallback_label);
        ImGui::EndDisabled();
    }

    if (!frame) {
        ImGui::PopStyleVar();
        ImGui::PopStyleColor(4);
    }

    return clicked;
}

bool egui_checkbox(const char* id, bool* value) {
    const float sz = EGUI_CHECKBOX_SIZE;
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(sz, sz));
    const bool pressed = ImGui::IsItemClicked();
    if (pressed) {
        *value = !*value;
    }

    const bool hovered = ImGui::IsItemHovered();
    ImDrawList* draw = ImGui::GetWindowDrawList();

    const ImU32 bg = hovered ? IM_COL32(70, 70, 70, 255) : IM_COL32(60, 60, 60, 255);
    draw->AddRectFilled(p, {p.x + sz, p.y + sz}, bg, 2.0f);
    draw->AddRect(p, {p.x + sz, p.y + sz}, IM_COL32(90, 90, 90, 255), 2.0f);

    if (*value) {
        const float pad = sz * 0.28f;
        const ImVec2 a{p.x + pad, p.y + sz * 0.52f};
        const ImVec2 b{p.x + sz * 0.42f, p.y + sz - pad};
        const ImVec2 c{p.x + sz - pad, p.y + pad};
        draw->AddLine(a, b, IM_COL32(220, 220, 220, 255), 1.4f);
        draw->AddLine(b, c, IM_COL32(220, 220, 220, 255), 1.4f);
    }

    return pressed;
}

bool egui_slider_int(const char* id, int* value, int v_min, int v_max, float width) {
    const float h = ImGui::GetFrameHeight();
    const ImVec2 p = ImGui::GetCursorScreenPos();

    ImGui::InvisibleButton(id, ImVec2(width, h));
    const bool active = ImGui::IsItemActive();
    const bool hovered = ImGui::IsItemHovered();

    const int old_value = *value;

    if (active && v_max > v_min) {
        const float t = std::clamp((ImGui::GetIO().MousePos.x - p.x) / std::max(width, 1.0f), 0.0f,
                                   1.0f);
        *value = v_min + static_cast<int>(t * static_cast<float>(v_max - v_min) + 0.5f);
    }

    const float rail_radius = std::max(h * 0.25f, 2.0f);
    const float cy = p.y + h * 0.5f;
    const float r_pad = h / 2.5f + 2.0f;

    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->PushClipRect({p.x - r_pad, cy - r_pad}, {p.x + width + r_pad, cy + r_pad}, false);

    dl->AddRectFilled({p.x, cy - rail_radius}, {p.x + width, cy + rail_radius},
                      IM_COL32(60, 60, 60, 255), 2.0f);

    const float frac =
        v_max > v_min ? std::clamp(static_cast<float>(*value - v_min) /
                                       static_cast<float>(v_max - v_min),
                                   0.0f, 1.0f)
                      : 0.0f;

    const float r = h / 2.5f;
    const ImVec2 c{p.x + frac * width, cy};
    dl->AddCircleFilled(c, r,
                        (hovered || active) ? IM_COL32(230, 230, 230, 255)
                                            : IM_COL32(200, 200, 200, 255));
    dl->AddCircle(c, r, IM_COL32(120, 120, 120, 255), 0, 1.0f);

    dl->PopClipRect();

    return *value != old_value;
}

bool number_field(const char* label, int* value, int min_value, int max_value, float width) {
    ImGui::TextUnformatted(label);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(width);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%d", *value);

    ImGui::PushID(label);
    const bool changed = ImGui::InputText("##v", buf, sizeof(buf),
                                          ImGuiInputTextFlags_CharsDecimal |
                                              ImGuiInputTextFlags_AutoSelectAll);
    ImGui::PopID();

    if (changed) {
        int parsed = *value;
        if (std::sscanf(buf, "%d", &parsed) == 1) {
            *value = std::clamp(parsed, min_value, max_value);
        }
    }

    return changed;
}

} // namespace andromeda::app::toolbar
