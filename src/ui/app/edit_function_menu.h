#pragma once

// ---------------------------------------------------------------------------
// Layer: ui
// Projects the editor-owned edit-function registry into the menu bar. Holds no
// knowledge of any concrete command: it renders whatever the registry declares.
// See docs/ARCHITECTURE.md section 13.
// ---------------------------------------------------------------------------

#include <functional>
#include <string_view>

#include "editor/edit_function_registry.h"

namespace andromeda::app {

/// Narrow dependency bundle for the projector. References plus one callback only -
/// never MainWindow*.
struct EditFunctionMenuContext {
    editor::EditFunctionRegistry& registry;
    editor::EditorController& controller;
    std::function<void(std::string_view function_id)> open_params_dialog;
};

/// Renders the registry entries belonging to one menu group.
class EditFunctionMenu {
public:
    explicit EditFunctionMenu(EditFunctionMenuContext context);

    /// Draws every entry whose menu path resolves to `menu_path`, in registration order.
    /// Must be called from inside an already-open menu.
    /// @param menu_path group to draw; an empty string resolves to default_group().
    void draw_group(std::string_view menu_path);

    /// @return true when at least one registered command resolves to this group, so the
    ///         caller can skip drawing an empty submenu.
    [[nodiscard]] bool has_group(std::string_view menu_path) const;

    /// The group used by entries whose declared menu_path is empty.
    [[nodiscard]] static std::string_view default_group() noexcept;

private:
    /// @return the group a registered entry belongs to, applying the default when unset.
    [[nodiscard]] static std::string_view effective_group(const editor::EditFunctionInfo& info);

    EditFunctionMenuContext m_context;
};

} // namespace andromeda::app
