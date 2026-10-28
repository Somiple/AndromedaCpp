#include "ui/app/edit_function_menu.h"

#include <imgui.h>

#include <utility>

#include "editor/editor_controller.h"

namespace andromeda::app {

namespace {

/// Groups are written "TopLevel/Sub". Anything deeper collapses to its first two segments so
/// the menu can never nest further than the single allowed submenu level.
[[nodiscard]] std::string_view normalise_group(std::string_view path) {
    const std::size_t first_sep = path.find('/');
    if (first_sep == std::string_view::npos) {
        return path;
    }
    const std::size_t second_sep = path.find('/', first_sep + 1);
    return second_sep == std::string_view::npos ? path : path.substr(0, second_sep);
}

} // namespace

EditFunctionMenu::EditFunctionMenu(EditFunctionMenuContext context) : m_context(std::move(context)) {}

std::string_view EditFunctionMenu::default_group() noexcept {
    // locked: commands without an explicit menu_path live under Tools -> Editing.
    return "Tools/Editing";
}

std::string_view EditFunctionMenu::effective_group(const editor::EditFunctionInfo& info) {
    return info.menu_path.empty() ? default_group() : normalise_group(info.menu_path);
}

bool EditFunctionMenu::has_group(std::string_view menu_path) const {
    const std::string_view target =
        menu_path.empty() ? default_group() : normalise_group(menu_path);

    for (const editor::EditFunctionInfo& info : m_context.registry.functions()) {
        if (effective_group(info) == target) {
            return true;
        }
    }
    return false;
}

void EditFunctionMenu::draw_group(std::string_view menu_path) {
    const std::string_view target =
        menu_path.empty() ? default_group() : normalise_group(menu_path);

    for (const editor::EditFunctionInfo& info : m_context.registry.functions()) {
        if (effective_group(info) != target) {
            continue;
        }

        const bool enabled = m_context.registry.is_enabled(info.id, m_context.controller);
        const char* shortcut = info.shortcut_label.empty() ? nullptr : info.shortcut_label.c_str();

        if (!ImGui::MenuItem(info.label.c_str(), shortcut, /*selected=*/false, enabled)) {
            continue;
        }

        // refactor: the registry decides whether the command needs input, so the menu no
        // longer carries a per-command dialog dispatch table.
        if (info.needs_params) {
            m_context.open_params_dialog(info.id);
        } else {
            m_context.registry.invoke(info.id, m_context.controller);
        }
        ImGui::CloseCurrentPopup();
    }
}

} // namespace andromeda::app
