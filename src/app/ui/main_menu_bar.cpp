#include "app/ui/main_menu_bar.h"

#include <imgui.h>

#include <utility>

namespace andromeda::app {

void MainMenuBar::add_menu(std::string name, std::vector<std::pair<std::string, MenuItem>> items) {
    menu_.push_back(TextMenu{std::move(name), std::move(items)});
}

void MainMenuBar::add_menu_image_action(unsigned int texture_id, float width, float height,
                                        MenuAction action) {
    menu_.push_back(ImageAction{texture_id, width, height, std::move(action)});
}

void MainMenuBar::add_custom(std::function<void()> content_fn) {
    menu_.push_back(CustomItem{ std::move(content_fn) });
}

void MainMenuBar::draw_menu_items(MainWindow& parent,
                                  std::vector<std::pair<std::string, MenuItem>>& menu_items) {
    for (auto& [label, menu_item] : menu_items) {
        std::visit(
            [&parent, &label](auto& item) {
                using T = std::decay_t<decltype(item)>;

                if constexpr (std::is_same_v<T, MenuButton>) {
                    if (ImGui::MenuItem(label.c_str())) {
                        if (item.action) {
                            item.action(parent);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                } else if constexpr (std::is_same_v<T, MenuSeparator>) {
                    ImGui::Separator();
                } else if constexpr (std::is_same_v<T, SubMenu>) {
                    if (ImGui::BeginMenu(label.c_str())) {
                        draw_menu_items(parent, item.items);
                        ImGui::EndMenu();
                    }
                } else if constexpr (std::is_same_v<T, MenuButtonEnabled>) {
                    const bool enabled = item.enabled ? item.enabled(parent) : true;
                    if (ImGui::MenuItem(label.c_str(), nullptr, false, enabled)) {
                        if (item.action) {
                            item.action(parent);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                } else {
                    static_assert(std::is_same_v<T, MenuButtonWithTooltip>);
                    if (ImGui::MenuItem(label.c_str())) {
                        if (item.action) {
                            item.action(parent);
                            ImGui::CloseCurrentPopup();
                        }
                    }
                    if (ImGui::IsItemHovered()) {
                        ImGui::SetTooltip("%s", item.tooltip.c_str());
                    }
                }
            },
            menu_item.node);
    }
}

void MainMenuBar::draw_menu(MainWindow& parent) {
    if (!ImGui::BeginMenuBar()) {
        return;
    }

    for (auto& entry : menu_) {
        std::visit(
            [&parent](auto& e) {
                using T = std::decay_t<decltype(e)>;

                if constexpr (std::is_same_v<T, TextMenu>) {
                    if (ImGui::BeginMenu(e.name.c_str())) {
                        draw_menu_items(parent, e.items);
                        ImGui::EndMenu();
                    }
                } else if constexpr(std::is_same_v<T, CustomItem>) {
                    if (e.content_fn) e.content_fn();
                } else {
                    static_assert(std::is_same_v<T, ImageAction>);
                    if (e.texture_id != 0) {
                        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(0.0f, 0.0f));
                        ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0, 0, 0, 0));
                        ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));

                        if (ImGui::ImageButton("##logo",
                                               static_cast<ImTextureID>(e.texture_id),
                                               ImVec2(e.width, e.height))) {
                            if (e.action) {
                                e.action(parent);
                            }
                        }

                        ImGui::PopStyleColor(2);
                        ImGui::PopStyleVar();
                    }
                }
            },
            entry);
    }

    ImGui::EndMenuBar();
}

}
