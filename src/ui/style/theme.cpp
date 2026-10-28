#include "ui/style/theme.h"

#include <imgui.h>

namespace andromeda::app {

namespace {

constexpr ImVec4 rgb(int r, int g, int b, int a = 255) {
    return ImVec4(static_cast<float>(r) / 255.0f, static_cast<float>(g) / 255.0f,
                  static_cast<float>(b) / 255.0f, static_cast<float>(a) / 255.0f);
}

}

void apply_egui_dark_theme(float scale) {
    ImGuiStyle& style = ImGui::GetStyle();
    style = ImGuiStyle();

    ImVec4* c = style.Colors;

    // Backgrounds: near-black with a barely noticeable violet undertone.
    c[ImGuiCol_WindowBg] = rgb(27, 26, 30);
    c[ImGuiCol_ChildBg] = rgb(27, 26, 30);
    c[ImGuiCol_PopupBg] = rgb(37, 35, 42, 245);
    c[ImGuiCol_MenuBarBg] = rgb(27, 26, 30);

    // Text: neutral gray, preserving the understated dark theme.
    c[ImGuiCol_Text] = rgb(200, 198, 204);
    c[ImGuiCol_TextDisabled] = rgb(120, 117, 126);

    // Borders: subtle cool violet-gray.
    c[ImGuiCol_Border] = rgb(61, 57, 68);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);

    // Frames: neutral until hovered, purple when interacted with.
    c[ImGuiCol_FrameBg] = rgb(59, 56, 64);
    c[ImGuiCol_FrameBgHovered] = rgb(79, 66, 94);
    c[ImGuiCol_FrameBgActive] = rgb(105, 77, 132);

    // Buttons: restrained purple, with brighter active states.
    c[ImGuiCol_Button] = rgb(66, 57, 77);
    c[ImGuiCol_ButtonHovered] = rgb(91, 68, 119);
    c[ImGuiCol_ButtonActive] = rgb(119, 83, 157);

    // Headers: selectable items, tree nodes, and similar widgets.
    c[ImGuiCol_Header] = rgb(66, 57, 77);
    c[ImGuiCol_HeaderHovered] = rgb(91, 68, 119);
    c[ImGuiCol_HeaderActive] = rgb(119, 83, 157);

    // Window title bars.
    c[ImGuiCol_TitleBg] = rgb(27, 26, 30);
    c[ImGuiCol_TitleBgActive] = rgb(39, 34, 47);
    c[ImGuiCol_TitleBgCollapsed] = rgb(27, 26, 30);

    // Separators.
    c[ImGuiCol_Separator] = rgb(61, 57, 68);
    c[ImGuiCol_SeparatorHovered] = rgb(112, 82, 143);
    c[ImGuiCol_SeparatorActive] = rgb(145, 99, 184);

    // Scrollbars.
    c[ImGuiCol_ScrollbarBg] = rgb(21, 20, 24);
    c[ImGuiCol_ScrollbarGrab] = rgb(61, 57, 68);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(91, 72, 111);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(119, 83, 157);

    // Sliders and checkboxes: purple accents.
    c[ImGuiCol_SliderGrab] = rgb(145, 111, 177);
    c[ImGuiCol_SliderGrabActive] = rgb(182, 139, 220);
    c[ImGuiCol_CheckMark] = rgb(184, 145, 220);

    // Resize grips.
    c[ImGuiCol_ResizeGrip] = rgb(66, 57, 77);
    c[ImGuiCol_ResizeGripHovered] = rgb(91, 68, 119);
    c[ImGuiCol_ResizeGripActive] = rgb(119, 83, 157);

    // Tables.
    c[ImGuiCol_TableHeaderBg] = rgb(39, 36, 45);
    c[ImGuiCol_TableBorderStrong] = rgb(61, 57, 68);
    c[ImGuiCol_TableBorderLight] = rgb(46, 43, 52);
    c[ImGuiCol_TableRowBg] = rgb(27, 26, 30, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 8);

    // Tabs: subtle at rest, purple when active.
    c[ImGuiCol_Tab] = rgb(38, 35, 44);
    c[ImGuiCol_TabHovered] = rgb(91, 68, 119);
    c[ImGuiCol_TabSelected] = rgb(76, 61, 93);
    c[ImGuiCol_TabSelectedOverline] = rgb(145, 99, 184);
    c[ImGuiCol_TabDimmed] = rgb(31, 29, 35);
    c[ImGuiCol_TabDimmedSelected] = rgb(48, 42, 57);

    // Navigation and windowing.
    c[ImGuiCol_NavCursor] = rgb(184, 145, 220);
    c[ImGuiCol_NavWindowingHighlight] = rgb(184, 145, 220, 40);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0, 0, 0, 60);

    // Selected text: translucent violet instead of cyan.
    c[ImGuiCol_TextSelectedBg] = rgb(112, 76, 151, 140);

    // Keep the original geometry.
    style.WindowRounding = 6.0f;
    style.ChildRounding = 4.0f;
    style.FrameRounding = 2.0f;
    style.PopupRounding = 4.0f;
    style.ScrollbarRounding = 4.0f;
    style.TabRounding = 4.0f;

    style.WindowBorderSize = 1.0f;
    style.ChildBorderSize = 1.0f;
    style.PopupBorderSize = 1.0f;
    style.FrameBorderSize = 1.0f;

    style.GrabRounding = 8.0f;

    style.WindowPadding = ImVec2(8.0f, 6.0f);
    style.FramePadding = ImVec2(6.0f, 3.0f);
    style.ItemSpacing = ImVec2(8.0f, 4.0f);
    style.ItemInnerSpacing = ImVec2(4.0f, 4.0f);
    style.ScrollbarSize = 12.0f;
    style.GrabMinSize = 12.0f;

    style.WindowTitleAlign = ImVec2(0.5f, 0.5f);

    style.ScaleAllSizes(scale);
}

}
