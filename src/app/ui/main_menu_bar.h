#pragma once

#include <functional>
#include <memory>
#include <string>
#include <variant>
#include <vector>

namespace andromeda::app {

class MainWindow;

struct MenuItem;

using MenuAction = std::function<void(MainWindow&)>;
using MenuEnabled = std::function<bool(MainWindow&)>;

struct MenuButton {
    MenuAction action;
};

struct MenuButtonEnabled {
    MenuAction action;
    MenuEnabled enabled;
};

struct MenuSeparator {};

struct SubMenu {
    std::vector<std::pair<std::string, MenuItem>> items;
};

struct MenuButtonWithTooltip {
    std::string tooltip;
    MenuAction action;
};

struct MenuItem {
    std::variant<MenuButton, MenuButtonEnabled, MenuSeparator, SubMenu, MenuButtonWithTooltip> node;

    MenuItem() = default;

    template <typename T>
        requires(!std::is_same_v<std::decay_t<T>, MenuItem>)
    MenuItem(T&& v) : node(std::forward<T>(v)) {}
};

class MainMenuBar {
public:
    void add_menu(std::string name, std::vector<std::pair<std::string, MenuItem>> items);

    void add_menu_image_action(unsigned int texture_id, float width, float height,
                               MenuAction action);

    void draw_menu(MainWindow& parent);

private:
    struct TextMenu {
        std::string name;
        std::vector<std::pair<std::string, MenuItem>> items;
    };

    struct ImageAction {
        unsigned int texture_id;
        float width;
        float height;
        MenuAction action;
    };

    static void draw_menu_items(MainWindow& parent,
                                std::vector<std::pair<std::string, MenuItem>>& menu_items);

    std::vector<std::variant<TextMenu, ImageAction>> menu_;
};

}
