#include "app/theme.h"

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

    c[ImGuiCol_WindowBg] = rgb(27, 27, 27);
    c[ImGuiCol_ChildBg] = rgb(27, 27, 27);
    c[ImGuiCol_PopupBg] = rgb(37, 37, 37, 245);
    c[ImGuiCol_MenuBarBg] = rgb(27, 27, 27);

    c[ImGuiCol_Text] = rgb(200, 200, 200);
    c[ImGuiCol_TextDisabled] = rgb(120, 120, 120);

    c[ImGuiCol_Border] = rgb(60, 60, 60);
    c[ImGuiCol_BorderShadow] = rgb(0, 0, 0, 0);

    c[ImGuiCol_FrameBg] = rgb(60, 60, 60);
    c[ImGuiCol_FrameBgHovered] = rgb(70, 70, 70);
    c[ImGuiCol_FrameBgActive] = rgb(85, 85, 85);

    c[ImGuiCol_Button] = rgb(60, 60, 60);
    c[ImGuiCol_ButtonHovered] = rgb(70, 70, 70);
    c[ImGuiCol_ButtonActive] = rgb(85, 85, 85);

    c[ImGuiCol_Header] = rgb(60, 60, 60);
    c[ImGuiCol_HeaderHovered] = rgb(70, 70, 70);
    c[ImGuiCol_HeaderActive] = rgb(85, 85, 85);

    c[ImGuiCol_TitleBg] = rgb(27, 27, 27);
    c[ImGuiCol_TitleBgActive] = rgb(37, 37, 37);
    c[ImGuiCol_TitleBgCollapsed] = rgb(27, 27, 27);

    c[ImGuiCol_Separator] = rgb(60, 60, 60);
    c[ImGuiCol_SeparatorHovered] = rgb(90, 90, 90);
    c[ImGuiCol_SeparatorActive] = rgb(110, 110, 110);

    c[ImGuiCol_ScrollbarBg] = rgb(20, 20, 20);
    c[ImGuiCol_ScrollbarGrab] = rgb(60, 60, 60);
    c[ImGuiCol_ScrollbarGrabHovered] = rgb(70, 70, 70);
    c[ImGuiCol_ScrollbarGrabActive] = rgb(85, 85, 85);

    c[ImGuiCol_SliderGrab] = rgb(200, 200, 200);
    c[ImGuiCol_SliderGrabActive] = rgb(240, 240, 240);
    c[ImGuiCol_CheckMark] = rgb(210, 210, 210);

    c[ImGuiCol_ResizeGrip] = rgb(60, 60, 60);
    c[ImGuiCol_ResizeGripHovered] = rgb(70, 70, 70);
    c[ImGuiCol_ResizeGripActive] = rgb(85, 85, 85);

    c[ImGuiCol_TableHeaderBg] = rgb(37, 37, 37);
    c[ImGuiCol_TableBorderStrong] = rgb(60, 60, 60);
    c[ImGuiCol_TableBorderLight] = rgb(45, 45, 45);
    c[ImGuiCol_TableRowBg] = rgb(27, 27, 27, 0);
    c[ImGuiCol_TableRowBgAlt] = rgb(255, 255, 255, 8);

    c[ImGuiCol_NavCursor] = rgb(0, 0, 0, 0);
    c[ImGuiCol_NavWindowingHighlight] = rgb(255, 255, 255, 40);
    c[ImGuiCol_NavWindowingDimBg] = rgb(0, 0, 0, 60);

    c[ImGuiCol_TextSelectedBg] = rgb(0, 92, 128, 140);

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
