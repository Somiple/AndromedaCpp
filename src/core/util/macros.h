#pragma once

// ---------------------------------------------------------------------------
// Small shared macros.
//
// `MAKE_GETTER` used to live in `ui/app/editor_app.h`, which meant any consumer
// of the macro had to include the whole app facade. It is a pure code-shape
// helper with no dependencies, so it belongs in the core layer where every layer
// may include it.
// ---------------------------------------------------------------------------

/// Declares a const and a non-const `[[nodiscard]]` getter pair over `value`.
/// @param type  Declared return type of the accessors.
/// @param name  Accessor name.
/// @param value Expression evaluated by both accessors.
#define MAKE_GETTER(type, name, value) \
	[[nodiscard]] type name() { return value; } \
	[[nodiscard]] const type name() const { return value; }
