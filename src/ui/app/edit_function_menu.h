#pragma once

// ---------------------------------------------------------------------------
// Layer: ui
// Projects the editor-owned edit-function registry into the menu bar. Holds no
// knowledge of any concrete command: it renders whatever the registry declares.
// See docs/ARCHITECTURE.md section 13.
// ---------------------------------------------------------------------------

#include <array>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "editor/edit_function_registry.h"

namespace andromeda::app {

/// Placement used when EditFunctionInfo::menu_path is empty.
inline constexpr std::string_view kDefaultMenuPath = "Tools/Editing";

/// Top-level menu names the bar emits. Must stay in sync with build_menu_bar(); a mismatch here is
/// exactly what the log-and-drop rule in resolve_path() catches. There is no MenuSection enum on
/// this branch, so a relocation target is a plain top-level name.
inline constexpr std::array<std::string_view, 7> kKnownSections{
    "File", "Edit", "View", "Project", "Tools", "Options", "Help"
};

/// One resolved placement bucket. An empty `group` means the commands sit directly in `section`
/// (a one-segment menu path); a non-empty `group` becomes a real submenu.
struct EditFunctionGroup {
    std::string section;
    std::string group;
    std::vector<const editor::EditFunctionInfo*> functions;
};

/// Narrow dependency bundle for the projector. References plus one callback only - never
/// MainWindow*.
struct EditFunctionMenuContext {
    editor::EditFunctionRegistry& registry;
    editor::EditorController& controller;
    std::function<void(std::string_view function_id)> open_params_dialog;
};

/// Renders the registry entries belonging to one menu section, creating one submenu per group.
class EditFunctionMenu {
public:
    /// Resolves every registered command's menu_path once. Registration is startup-only and the
    /// bar is composed a single time, so the grouping never needs invalidating, and an invalid
    /// path is reported once here instead of on every frame.
    /// @param context Non-owning services; the registry and the controller must outlive this
    ///                object.
    explicit EditFunctionMenu(EditFunctionMenuContext context);

    /// @return the resolved buckets, in first-seen registration order.
    [[nodiscard]] const std::vector<EditFunctionGroup>& groups() const noexcept { return m_groups; }

    /// @return the distinct section names that currently hold at least one command.
    [[nodiscard]] std::vector<std::string_view> sections() const;

    /// @return true when at least one command is placed in this top-level menu.
    [[nodiscard]] bool has_section(std::string_view section) const;

    /// @return true when this exact section/submenu bucket exists.
    [[nodiscard]] bool has_group(std::string_view section, std::string_view group) const;

    /// Draws everything placed in @p section: the flat entries first, then one submenu per group
    /// in registration order. No-op when the section holds nothing.
    /// @param section top-level menu name, e.g. "Tools" or "Edit".
    /// Must be called from inside an already-open menu.
    void draw_section(std::string_view section);

    /// Draws one submenu's items, without opening the submenu. No-op when the group is absent.
    /// @param section top-level menu name.
    /// @param group submenu name; the empty string addresses the section's flat entries.
    void draw_group(std::string_view section, std::string_view group);

private:
    /// One validated menu placement.
    struct ResolvedPath {
        std::string section;  ///< top-level menu name
        std::string group;    ///< submenu name; empty when the command sits in the section itself
    };

    /// Resolves a raw menu_path into a placement.
    /// Accepts an empty path (the default), one non-empty segment, or two non-empty segments whose
    /// first part is a known top-level name.
    /// @param path raw EditFunctionInfo::menu_path.
    /// @param id command id, used only for the error message.
    /// @return the placement, or nullopt after logging exactly one error when the path is
    ///         malformed. A rejected command stays registered and invocable by id; it is only
    ///         left out of every menu.
    [[nodiscard]] static std::optional<ResolvedPath> resolve_path(std::string_view path,
                                                                 std::string_view id);

    /// Draws one leaf entry: enable predicate, dialog-or-invoke dispatch, popup close.
    void draw_entry(const editor::EditFunctionInfo& info);

    EditFunctionMenuContext m_context;
    std::vector<EditFunctionGroup> m_groups;
};

} // namespace andromeda::app
