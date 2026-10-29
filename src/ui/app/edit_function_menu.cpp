#include "ui/app/edit_function_menu.h"

#include <imgui.h>

#include <algorithm>
#include <format>
#include <utility>

#include "core/debugger.h"
#include "editor/editor_controller.h"

namespace andromeda::app {

namespace {

/// Trims ASCII whitespace off both ends; an all-whitespace input yields an empty view.
[[nodiscard]] std::string_view trim_ascii(std::string_view text) {
    constexpr std::string_view kWhitespace = " \t\r\n";
    const std::size_t first = text.find_first_not_of(kWhitespace);
    if (first == std::string_view::npos) {
        return {};
    }
    const std::size_t last = text.find_last_not_of(kWhitespace);
    return text.substr(first, last - first + 1);
}

/// @return true when `section` names one of the top-level menus the bar actually emits.
[[nodiscard]] bool is_known_section(std::string_view section) {
    return std::any_of(kKnownSections.begin(), kKnownSections.end(),
                       [section](std::string_view known) { return known == section; });
}

} // namespace

EditFunctionMenu::EditFunctionMenu(EditFunctionMenuContext context) : m_context(std::move(context)) {
    // refactor: every registration is resolved and bucketed exactly once. registration is
    // startup-only and the bar is composed a single time, so this grouping never needs
    // invalidating, and a malformed path is reported once here instead of on every frame.
    for (const editor::EditFunctionInfo& info : m_context.registry.functions()) {
        const std::optional<ResolvedPath> resolved = resolve_path(info.menu_path, info.id);
        if (!resolved) {
            continue;   // already logged in resolve_path; the command stays invocable by id
        }

        const auto existing = std::find_if(m_groups.begin(), m_groups.end(),
            [&resolved](const EditFunctionGroup& group) {
                return group.section == resolved->section && group.group == resolved->group;
            });

        if (existing == m_groups.end()) {
            m_groups.push_back(EditFunctionGroup{ resolved->section, resolved->group, {} });
            m_groups.back().functions.push_back(&info);
        } else {
            existing->functions.push_back(&info);
        }
    }
}

std::vector<std::string_view> EditFunctionMenu::sections() const {
    std::vector<std::string_view> result;
    for (const EditFunctionGroup& group : m_groups) {
        const std::string_view section = group.section;
        if (std::find(result.begin(), result.end(), section) == result.end()) {
            result.push_back(section);
        }
    }
    return result;
}

bool EditFunctionMenu::has_section(std::string_view section) const {
    return std::any_of(m_groups.begin(), m_groups.end(),
                       [section](const EditFunctionGroup& group) {
                           return group.section == section;
                       });
}

bool EditFunctionMenu::has_group(std::string_view section, std::string_view group) const {
    return std::any_of(m_groups.begin(), m_groups.end(),
                       [section, group](const EditFunctionGroup& candidate) {
                           return candidate.section == section && candidate.group == group;
                       });
}

void EditFunctionMenu::draw_section(std::string_view section) {
    // refactor: the bar no longer names a group. it hands over the section and the projector
    // decides which commands belong here and whether a submenu has to be created, so a command
    // that overrode its menu_path is relocated instead of disappearing.
    std::vector<const EditFunctionGroup*> matching;
    for (const EditFunctionGroup& group : m_groups) {
        if (group.section == section) {
            matching.push_back(&group);
        }
    }
    if (matching.empty()) {
        return;
    }

    bool wrote_anything = false;

    // pass 1: commands with a one-segment path sit directly in the section.
    for (const EditFunctionGroup* group : matching) {
        if (!group->group.empty()) {
            continue;
        }
        for (const editor::EditFunctionInfo* info : group->functions) {
            draw_entry(*info);
            wrote_anything = true;
        }
    }

    // pass 2: one real submenu per group, in first-seen registration order.
    for (const EditFunctionGroup* group : matching) {
        if (group->group.empty()) {
            continue;
        }
        if (wrote_anything) {
            ImGui::Separator();
        }
        if (ImGui::BeginMenu(group->group.c_str())) {
            for (const editor::EditFunctionInfo* info : group->functions) {
                draw_entry(*info);
            }
            ImGui::EndMenu();
        }
        wrote_anything = true;
    }
}

void EditFunctionMenu::draw_group(std::string_view section, std::string_view group) {
    const auto found = std::find_if(m_groups.begin(), m_groups.end(),
        [section, group](const EditFunctionGroup& candidate) {
            return candidate.section == section && candidate.group == group;
        });

    if (found == m_groups.end()) {
        return;   // absent group: nothing to draw and nothing to log at frame rate
    }

    for (const editor::EditFunctionInfo* info : found->functions) {
        draw_entry(*info);
    }
}

void EditFunctionMenu::draw_entry(const editor::EditFunctionInfo& info) {
    const bool enabled = m_context.registry.is_enabled(info.id, m_context.controller);
    const char* shortcut = info.shortcut_label.empty() ? nullptr : info.shortcut_label.c_str();

    if (!ImGui::MenuItem(info.label.c_str(), shortcut, /*selected=*/false, enabled)) {
        return;
    }

    // refactor: the registry decides whether the command needs input, so the menu carries no
    // per-command dialog dispatch table.
    if (info.needs_params) {
        if (m_context.open_params_dialog) {
            m_context.open_params_dialog(info.id);
        }
    } else {
        m_context.registry.invoke(info.id, m_context.controller);
    }
    ImGui::CloseCurrentPopup();
}

std::optional<EditFunctionMenu::ResolvedPath> EditFunctionMenu::resolve_path(std::string_view path,
                                                                           std::string_view id) {
    // refactor: placement used to be an exact string compare against one hard-coded group, so any
    // override resolved to nothing and the command silently vanished. this is now the single
    // strict rule: the default, one segment, or two non-empty segments naming a known top-level
    // menu. anything else is a registration fault and is reported once.
    std::string_view raw = trim_ascii(path);
    if (raw.empty()) {
        raw = kDefaultMenuPath;
    }

    std::vector<std::string_view> parts;
    std::size_t begin = 0;
    while (true) {
        const std::size_t separator = raw.find('/', begin);
        const std::size_t end = separator == std::string_view::npos ? raw.size() : separator;
        const std::string_view token = trim_ascii(raw.substr(begin, end - begin));

        if (token.empty()) {
            // catches "Tools/" (trailing slash), "/Tools" (leading) and "Edit//Notes" (hollow).
            util::Debugger::log_error(std::format(
                "edit command '{}' has an invalid menu path '{}' - it is not shown in any menu",
                id, path));
            return std::nullopt;
        }

        parts.push_back(token);

        if (separator == std::string_view::npos) {
            break;
        }
        begin = separator + 1;
    }

    if (parts.size() > 2 || !is_known_section(parts.front())) {
        util::Debugger::log_error(std::format(
            "edit command '{}' has an invalid menu path '{}' - it is not shown in any menu",
            id, path));
        return std::nullopt;
    }

    ResolvedPath resolved;
    resolved.section = std::string(parts.front());
    if (parts.size() == 2) {
        resolved.group = std::string(parts.back());
    }
    return resolved;
}

} // namespace andromeda::app
