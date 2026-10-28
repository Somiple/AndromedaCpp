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

/// Draws arbitrary content inside an open menu. Used to project a dynamic list (the
/// edit-function registry) into a submenu without rebuilding the static table.
struct MenuCustomContent {
    std::function<void(MainWindow&)> draw;
};

struct MenuItem {
    std::variant<MenuButton, MenuButtonEnabled, MenuSeparator, SubMenu, MenuButtonWithTooltip,
                 MenuCustomContent>
        node;

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

    void add_custom(std::function<void()> content_fn);

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

    struct CustomItem {
        std::function<void()> content_fn;
    };

    static void draw_menu_items(MainWindow& parent,
                                std::vector<std::pair<std::string, MenuItem>>& menu_items);

    /// Visual-only search stub drawn flush right on the menu row. Read-only so it can never
    /// capture keystrokes the editor needs; filtering is not wired yet.
    void draw_search_stub();

    std::vector<std::variant<TextMenu, ImageAction, CustomItem>> menu_;

    char m_search_buffer[128] = {};
};

}
