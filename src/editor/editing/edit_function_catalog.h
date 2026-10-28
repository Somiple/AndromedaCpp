#pragma once

// ---------------------------------------------------------------------------
// Layer: editor
// The single extension point for editor commands: add one registration block to
// `edit_function_catalog.cpp` and the command becomes available to the registry,
// the menu projection and the parameters dialog.
// See docs/ARCHITECTURE.md section 13.
// ---------------------------------------------------------------------------

namespace andromeda::editor {

class EditFunctionRegistry;

/// Registers every built-in editor command.
/// @param registry registry to populate; call once during controller construction.
void register_builtin_edit_functions(EditFunctionRegistry& registry);

} // namespace andromeda::editor
