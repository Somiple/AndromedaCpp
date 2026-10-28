#pragma once

// REFACTOR: EditorContext / KeyModifierState / ViewSnapshot / TickView moved into the
// editor layer (editor/editor_context.h, editor/tick_view.h) so that src/editor/** no
// longer includes ui/app/view_settings.h or names managers::PlaybackManager.
//
// This header is kept as a forwarding shim: every existing `andromeda::app::X` spelling
// keeps compiling unchanged, and the include direction (ui -> editor) is the legal one.
#include "editor/editor_context.h"
#include "editor/tick_view.h"

namespace andromeda::app {

using editor::EditorContext;
using editor::KeyModifierState;
using editor::TickView;
using editor::ViewSnapshot;

} // namespace andromeda::app
