# Andromeda — Architecture & Conventions

This document is the contract for where code lives, what may depend on what, and
which conventions new code must follow. It describes the tree **as it is now**,
after the modular re-layout; older paths (`src/app/`, `src/util/`, one monolithic
`panels.cpp`) no longer exist.

---

## 1. Layer map

Dependency direction — **left may never include right**:

```
core  ->  midi  ->  editor  ->  audio  ->  ui
```

| Layer | Directory | Responsibility | May include |
|---|---|---|---|
| **core** | `src/core/` | Foundation with no project dependencies: logging (`debugger.h`), crash handler, system stats, expression parser, `timer.h`, `numeric.h`, `util.h`, `macros.h`, `shared.h` (`Shared<T>` / `SharedMut<T>`), `math/`, `collections/`. | nothing |
| **midi** | `src/midi/` | MIDI file model and I/O: `midi_types.h`, `midi_file.h`, `midi_track.h`, `events/` (note, meta, channel), `io/` (buffered reader, mapped file), `export/` (plan → encode → pipeline → write). | `core/` |
| **editor** | `src/editor/` | The editor itself: `EditorController`, `EditorComponent` + the four editing passes (`editing/note_editing`, `data_editing`, `meta_editing`, `track_editing`), `project/` (`ProjectData`, `ProjectManager`), `settings/`, `history/` (transaction, action history, executor), `plugins/` (Lua host), `timeline/`, `overlays/`, `navigation`, `edit_function_registry` + `editing/edit_function_catalog`. | `core/`, `midi/` |
| **audio** | `src/audio/` | Playback: `audio_timer` (the playhead), `midi_out`, `midi_audio`, `realtime_engine`, `event_producer`, `maestro_engine`, vendored `maestro.h`. | `core/`, `midi/`, `editor/` |
| **ui** | `src/ui/` | Everything ImGui / GLFW / OpenGL: `app/` (window, app facade, managers, MIDI I/O, options, view settings), `rendering/`, `widgets/`, `panels/`, `dialogs/`, `layout/`, `style/`, `util/` (image loading). | everything below |

`src/main.cpp` is the only file outside a layer; it parses the command line and
runs `MainWindow`.

### Layer sub-structure (`src/ui/`)

| Folder | Contents |
|---|---|
| `ui/app/` | `main_window.*`, `editor_app.*`, `managers.*` (`PlaybackManager`), `project_actions.*` (project + MIDI flows), `app_options.h`, `view_settings.h`, `app_event_listener.h`, `editor_tool.h` |
| `ui/rendering/` | One translation unit per view: `piano_roll.cpp`, `track_view.cpp`, `data_view.cpp`, plus shared GPU plumbing (`buffers`, `shaders`, `note_gpu_cache`, `note_uploader`, `note_occlusion`, `screen_coverage`, `note_cull_helper`), `rendering.h` |
| `ui/widgets/` | `EditorWidget` base + the three concrete widgets, `toolbar_widgets.*`, `music_glyph.*`, `custom_widgets.h`, `timeline_nav.h` |
| `ui/panels/` | One translation unit per toolbar domain (see §3) |
| `ui/dialogs/` | `dialog.h`, `dialog_manager`, `dialog_drawer`, `main_menu_bar`, `filter_channels`, `editor_info`, `manual`, `simple_dialog`, `crash_dialog`, `ui_helpers.h` |
| `ui/layout/` | `dock_manager`, `dock_panel.h`, `panel_layout` |
| `ui/style/` | `theme`, `note_colors` (the note palette + its GPU texture) |
| `ui/util/` | `image_loader`, `image_handle.h`, `native_file_dialog` (shell open/save picker) |

---

## 2. Build conventions (`CMakeLists.txt`)

- **Include root is `src`.** Every project include is written relative to it:
  `"core/debugger.h"`, `"editor/timeline/timeline.h"`, `"ui/panels/panels.h"`.
  Never use a path that only works because of the including file's own folder.
- **`andromeda_core` (STATIC) holds every `.cpp`.** The `andromeda` executable is
  `src/main.cpp` alone and links `andromeda_core`. Anything extracted out of
  `main.cpp` belongs in the library.
- **A new `.cpp` MUST be added to the `andromeda_core` source list**, otherwise it
  is silently not compiled and the link fails with unresolved externals.
  **A new `.h` needs no CMake change** — headers are implicit.
- vcpkg provides all third-party dependencies. Do not add new ones; the allowed set
  is ImGui, GLFW, OpenGL/glad, sol2/Lua, RtMidi, curl, nlohmann_json and Stb.
- There is **no `enable_testing()` / `add_test()`** in this project, so a test run
  builds but reports zero tests. A successful build is the real compile gate.

---

## 3. Panel translation-unit convention (`src/ui/panels/`)

Panels were split out of one 1.8 k-line `panels.cpp`. Every panel TU follows the
same shape:

```cpp
#include "ui/panels/panels.h"

#define IMGUI_DEFINE_MATH_OPERATORS   // must precede <imgui.h>
#include <imgui.h>
#include <imgui_internal.h>           // only when ImGuiWindow::DC is needed

// ... standard headers ...

#include "ui/app/main_window.h"
#include "ui/widgets/toolbar_widgets.h"

namespace andromeda::app {

using toolbar::icon_button;
using toolbar::number_field;
using toolbar::toolbar_separator;
// ... etc.

} // namespace andromeda::app
```

Rules:

- `#define IMGUI_DEFINE_MATH_OPERATORS` **before** `<imgui.h>` — `ImVec2` operators
  are used pervasively.
- `imgui_internal.h` only where `ImGuiWindow::DC` / internal APIs are genuinely
  needed.
- `ui/panels/panels.h` is the aggregate declaration header for every
  `draw_panel_*` entry point. It **forward-declares `MainWindow` only** — never
  include `main_window.h` from it.
- Panel entry points take `MainWindow&` and reach subsystems through the
  `MainWindow` accessors (§4), never through data members.
- The `using toolbar::...` block replaces the anonymous namespace the old monolith
  used, so panel bodies still read `icon_button(...)`.

---

## 4. `MainWindow` rule: accessors, never public data

`MainWindow` owns the window, the dock layout, the widgets, the GL surface and the
dialog system. It has **no public data members**. Storage is private, grouped in
`#pragma region Subsystem storage`; every subsystem is reached through an accessor
with a `///` doc comment.

Accessor → storage map:

| Accessor | Storage / effect |
|---|---|
| `controller()` | `editor_controller` |
| `navigation()` | `nav` (`SharedPtr<PianoRollNavigation>`) |
| `track_navigation()` | `track_nav` (`SharedPtr<TrackViewNavigation>`) |
| `settings()` | `view_settings->value` (**no lock taken** — UI thread only) |
| `view_settings_handle()` | `view_settings` (the `SharedMut` itself, for locking) |
| `plugins()` | `plugin_loader` |
| `dialogs()` | `*dialog_manager` |
| `docks()` | `*dock_manager` |
| `dialog_drawer()` | `dialog_drawer_` |
| `colors()` | `*note_colors` |
| `note_color_indexing()` / `set_note_color_indexing()` | `note_color_indexing_` |
| `note_culler()` | `note_culler_` |
| `views()` | `widgets` |
| `uploader()` | `note_uploader` |
| `scroll_return_pos()` | `scroll_return_pos_` |
| `project_actions()` | `*project_actions_` — project/MIDI lifecycle coordinator, see §11 |
| `events()` | `app_event_handler` |
| `images()` | `image_resources` |
| `stats()` | `sys_stats` |
| `render_type()` / `set_render_type()` | `render_type_` (`set_` carries the view position over) |
| `is_mouse_over_ui()` / `set_mouse_over_ui()` | `mouse_over_ui_` |
| `get_mouse_pos()` / `get_key_modifier_state()` | `_mouse_pos` / `_key_modifiers` |
| `get_central_tick_view()` | `active_view().true_tick_view()` |

Notes:

- Members that collided with their accessor name got a trailing underscore
  (`dialog_drawer_`, `note_color_indexing_`, `render_type_`, `note_culler_`,
  `scroll_return_pos_`, `mouse_over_ui_`); the rest kept their original names.
- `settings()` deliberately returns the value **without** taking the mutex. That is
  the existing contract: panels and the GL pass share the UI thread. Do not add
  locking inside it.
- `set_render_type()` is the only way to switch the central view; it carries the
  view position over, so assigning `render_type_` directly would lose it.

---

## 5. `EditorApp` vs `MainWindow`

- `EditorApp` owns the editor domain: `EditorController`, `PluginLoader`,
  `PianoRollNavigation`, `TrackViewNavigation`, `ViewSettings`,
  `EventListenerHandler`, and the audio stack (`AudioTimer`, `MIDIAudio`,
  `PlaybackManager`).
- `MainWindow` owns the *window*: GLFW/GL, ImGui context, dock layout, widgets,
  dialogs, image resources, system stats, input state.
- Construction order is load-bearing: `EditorApp`'s constructor builds the cheap
  domain pieces only. `MainWindow` calls `_app->initialize()` **at the end of its
  constructor body**, because the audio engine and the playback manager need a fully
  constructed window.
- `EditorApp` never creates the audio engine in its constructor.

---

## 6. `EditorComponent` and the editing passes

- `EditorComponent(app::MainWindow* app, EditorController* controller)` is the base
  for `NoteEditing`, `DataEditing`, `MetaEditing`, `TrackEditing`. Derived classes
  use `using EditorComponent::EditorComponent;`, so a base-constructor change
  re-plumbs all four at once.
- Components reach the window **only through accessors** (`_app->settings()`,
  `_app->navigation()->value`, `_app->is_mouse_over_ui()`, …). The `MainWindow*`
  member is a compatibility remnant; do not add new direct data access.
- Dirty tracking lives in the base: `mark_edited()` on change,
  `has_edited_recently()` to consume the latch once per frame.
- `EditorController` owns project data, the editing passes, history, selection,
  clipboard, active track, tool/toolbar settings. Editing operations that need a
  dialog (`insert_meta`, `filter_selection_channels`) live here and borrow the
  dialog manager via `set_dialog_manager()` — the controller does **not** own it.

---

## 7. Include hygiene

Heavy or leaf-only dependencies must not leak through public headers:

- `ImageHandle` is in **`ui/util/image_handle.h`**; consumers that only pass handles
  around include that, not `ui/util/image_loader.h` (which owns the texture map).
- `TimelineGridSnap` and `SnapModifier` are in **`editor/timeline/snap_types.h`**;
  `timeline.h` includes it, so existing include sites are unchanged, but glyph
  rendering and panels only need the small header.
- `MAKE_GETTER` is in **`core/util/macros.h`**, not in `editor_app.h`.
- `AppOptions` (launch/benchmark options) is in **`ui/app/app_options.h`**;
  `MainWindow::StartupOptions` remains as an alias for existing call sites.
- `ui/panels/panels.h` and the widget headers forward-declare `MainWindow`.
- Headers are self-contained: include what you name. Do not rely on transitive
  includes, but do keep leaf types in small headers so a `.cpp` that only needs
  them does not pull a whole subsystem.

---

## 8. Conventions for new code

- **Comments.** Public declarations carry Doxygen `///` comments stating purpose,
  parameters, return values and anything a caller must know. Non-obvious logic gets
  an inline `//` explaining *why*, not a line-by-line translation.
- **Traceability.** Structural changes are marked with `// REFACTOR:` so the reason a
  file/function moved stays discoverable.
- **Naming.** `camelCase` methods, `PascalCase` types, private members `_snake_case`
  (or a trailing underscore where it avoids clashing with an accessor).
- **Namespace.** `andromeda::<layer>` — `andromeda::util`, `andromeda::midi`,
  `andromeda::editor`, `andromeda::audio`, `andromeda::app`.
- **No behaviour changes during refactors.** Movement is not permission to
  "simplify". In particular the selection/cursor anchor logic (the `left anchor`
  fix) must be moved verbatim, never reordered.
- **Shared state.** `util::Shared<T>` (readers-writer) and `util::SharedMut<T>`
  (exclusive) wrap state that crosses threads. Call sites read `.value` directly
  and lock explicitly around it; do not invent a new locking scheme inside them.

---

## 9. Tooling notes for contributors (and agents)

- **Visual Studio CodeModel is unavailable for this CMake + vcpkg project**, so
  `get_file_symbols` and symbol-index lookups fail for these sources. Use text
  search scoped to `src/` plus ranged file reads instead.
- **Do not search or scan `build/`** — `vcpkg_installed` contains thousands of
  third-party headers and drowns any glob.
- The off-screen benchmark is developer tooling: `MainWindow::run_render_bench()`
  runs only when `AppOptions::bench_frames > 0`, writes an optional PNG via Stb and
  prints timing tables to stdout. It is not part of shipped behaviour.

---

## 10. Adding a feature — checklist

1. Pick the layer: model/editing → `editor/`, playback → `audio/`, presentation →
   `ui/`.
2. New `.cpp`? Add it to the `andromeda_core` list in `CMakeLists.txt`.
3. Reaching a subsystem from a panel/widget/renderer? Add or use a `MainWindow`
   accessor — never a public data member.
4. New editing operation that needs a dialog? Put it on `EditorController` and use
   the injected `DialogManager*`.
5. Header-only dependency on a leaf type? Give it its own small header rather than
   pulling the owning subsystem.
6. Comment the public surface (`///`) and mark structural moves (`// REFACTOR:`).

---

## 11. Responsibility map — misfiled code and its destination

Recorded before the moves so every later diff stays traceable. Rows marked **done**
have already been carried out; the rest are pending and are marked in code with
`// refactor (todo): <destination>`.

| What was misfiled | Where it lived | Destination |
|---|---|---|
| project lifecycle (new / open / save / save-as), MIDI import + export, and the PPQ query | ~~`MainWindow::make_new_project`, `open_project`, `save_project`, `save_project_as`, `import_midi`, `export_midi`, `get_ppq`~~ | **done** — `app::ProjectActions` (`ui/app/project_actions.*`); `MainWindow::get_ppq()` remains only as a forwarder |
| MIDI byte-level writing | ~~`ui/app/midi_io.cpp`~~ | **done** — `midi::MidiFileIo` (`midi/io/midi_file_io.*`) |
| native shell file picker | ~~file-local helper in `ui/app/midi_io.cpp`~~ | **done** — `app::native_open_file_dialog` / `native_save_file_dialog` (`ui/util/native_file_dialog.*`) |
| dropped-file queue and parse-status polling | ~~`MIDIIoHandler`~~ | **done** — `app::ProjectActions` (`on_files_dropped`, `handle_dropped_files`, `take_last_parse_status`) |
| obsolete mixed-responsibility handler | ~~`ui/app/midi_io.{h,cpp}`~~ | **done** — both files deleted; `app::ProjectActions`, `midi::MidiFileIo` and `app::native_open_file_dialog` / `native_save_file_dialog` own the pieces |
| view switching: two renderer-activation paths | ~~`MainWindow::switch_view` + `set_render_type`~~ | **done** — `app::ViewCoordinator` (`ui/app/view_coordinator.*`); `switch_view` / `set_render_type` remain only as forwarders |
| active-track fact written twice | ~~`MainWindow::on_current_track_changed`~~ | **done** — `app::ViewCoordinator::on_current_track_changed` is the single writer of `PianoRollNavigation::curr_track`; the `pr_curr_track` mirror the audit found no longer exists |
| playback transport synchronisation inlined in a draw function | ~~`ui/panels/playback_panels.cpp` (`TODO: mutex`, dead commented transport block)~~ | **done** — the stale `TODO: mutex` was resolved as needing no lock; the dead block and the empty placeholder were removed |
| dock solver bookkeeping exposed as public fields | ~~`ui/layout/dock_panel.h` (`grip_held`, `measured`, `last_node_id`, `in_layout`, `was_active`, `thickness`, `thickness_axis`)~~ | **done** — private with accessors; `ui/layout/dock_manager.cpp` drives them through setters/getters |
| editor commands hard-wired as one enum plus one giant switch | ~~`editor/edit_functions.{h,cpp}` (`enum class EditFunction` + `EditFunctions::apply_function`)~~ | **done** — `editor::EditFunctionRegistry` (`editor/edit_function_registry.*`) seeded by `editor/editing/edit_function_catalog.*`, projected into the menu by `ui/app/edit_function_menu.*`; see §13 |
| one bespoke parameter dialog per command | ~~`EFStretchDialog`, `EFChopDialog`, `EFGlueDialog`, `EFSetChannelDialog`~~ | **done** — a single generic `EditFunctionParamsDialog` renders any command's declared `EditParam` list; all four classes and their `DIALOG_NAME_EF_*` constants were deleted |
| selection-channel filtering reachable two ways | ~~`MainWindow::filter_selection_channels` (stale forwarder)~~ | **done** — forwarder deleted; the real implementation is registry entry `selection.filter_channels` |

Rules the moves follow:

- A coordinator never takes `MainWindow*`; it takes a narrow services struct of
  references to the subsystems it actually uses.
- `MainWindow` keeps same-named forwarders so existing call sites do not churn.
- The layer rule still applies: `editor::PlaybackSync` may not include ImGui.
- Behaviour is preserved; the optional data-view activation in `switch_view` is kept
  because it targets a distinct renderer, not a duplicate of the central one.

Marker convention: structural markers added from §11 onwards are lowercase
(`// refactor:` / `// refactor (todo):`). Older `// REFACTOR:` markers predate this
and stay as they are.

---

## 12. Coordinators (`ui/app`)

Each coordinator owns one distinct domain and takes a narrow bundle of references plus the
few callbacks for work it must *cause* rather than own. None of them may take a
`MainWindow*`; `MainWindow` keeps same-named forwarders so call sites do not churn.

| Coordinator | Owns | Consumes |
|---|---|---|
| `ProjectActions` (`project_actions.*`) | project sessions: new/save, MIDI import + export, dropped-file queue, parse-status polling, the PPQ query | `editor::ProjectManager&` plus `open_dialog` / `on_project_reset` / `set_status` callbacks |
| `ViewCoordinator` (`view_coordinator.*`) | view switching, the only renderer-activation path, active-track forwarding, viewport carry-over between views | navigation handles `PianoRollNavigation` / `TrackViewNavigation`, `editor::EditorController&`, the window's render-type storage, the three widgets via `set_widgets()` |
| `DialogCoordinator` (`dialog_coordinator.*`) | dialog open policy and closed-dialog routing | `DialogManager&` plus a `handle_confirmed_id` callback routed back to `ProjectActions` |

The dialog factory table deliberately stays in `MainWindow::init_dialogs()`: naming the
concrete dialog types is window-layer knowledge, and moving it would pull a dozen dialog
headers into `dialog_coordinator.cpp` for no separation gain.

Single-owner invariants established by this refactor:

- The selected track lives only on `EditorController` (`get_active_track()` /
  `set_active_track()`). The `PianoRollNavigation::curr_track` and
  `ViewSettings::pr_curr_track` mirrors were deleted, and the per-frame
  `editor::ViewSnapshot::current_track` is filled from the controller.
- Renderer activation is owned solely by `ViewCoordinator::set_render_type`. It activates
  the data-view renderer together with the piano roll, because each widget builds its own
  renderer object; that is a distinct renderer, not a duplicate activation.
- `ProjectManager`'s model is private. `get_project_data()`, `get_project_info()`,
  `get_tracks()`, `get_timeline()` and `get_metas()` are the only access paths, and
  `ProjectData::mute_revision` keeps its contract: bumped by `set_track_muted` /
  `solo_track`, polled by the audio engines.

---

## 13. Edit-function registry (`editor/`, projected by `ui/app/`)

Adding an editor command is one registration block in
`src/editor/editing/edit_function_catalog.cpp` — the single file to edit:

```cpp
auto& function_builder = registry.add_function({ "note.example", "Example Operation", "" });
function_builder.set_action([](EditorController& controller) { /* ... */ });
```

A command that needs input declares it instead of shipping a dialog:

```cpp
function_builder.add_param({ .id = "amount", .label = "Semitones",
                             .type = EditParamType::Integer, .default_int = 0 });
function_builder.set_action([](EditorController& c, const EditFunctionArgs& args) { /* ... */ });
```

`set_action` now takes `EditorController&` (plus an optional `EditFunctionArgs`); the previous
`EditFunction` enum, its payload structs, the `EditFunctions` switch and the dead
`EditorController*` payload member are all gone. `MainWindow::apply_function` takes a
`std::string_view` id and the registry does the lookup.

| Piece | Where | Responsibility |
|---|---|---|
| `EditFunctionRegistry`, `EditFunctionBuilder`, `EditParam`, `EditFunctionArgs`, `EditFunctionInfo` | `editor/edit_function_registry.*` | owns the commands; rejects empty/duplicate ids; enforces the parameter contract |
| `register_builtin_edit_functions()` | `editor/editing/edit_function_catalog.*` | the one place a built-in command is added |
| `EditFunctionMenu` | `ui/app/edit_function_menu.*` | projects registered commands into the menu bar |
| `EditFunctionParamsDialog` | `ui/dialogs/edit_function_params_dialog.*` | renders any declared `EditParam` list |

**Layer split.** The registry lives in `editor/` and includes no ImGui, GLFW or `ui/` header,
so a command can be registered and invoked headlessly. Only the *projection* is UI.

**Ambient state.** A command must never ask the user for positional state the application
already owns. Transport state is read from `EditorController`: `playback()` returns the
nullable `IPlaybackProvider*` that the UI layer installs once from
`MainWindow::update_input_state`, and `playhead_tick()` returns
`IPlaybackProvider::snapshot().playhead_tick` rounded to a whole `MIDITick`, or `0` when no
provider is installed. `0` therefore means both "no playhead" and "the timeline start", so a
command guards on it and skips instead of cutting at tick zero. `snapshot()` samples the
transport and is not safe to call from the audio thread; the playhead accessor is the editing
path. Nothing in `editor/` may reach a `MainWindow` or a `PlaybackManager` to obtain this.

Worked example: `note.slice_at_tick` takes no parameter and cuts only the notes the playhead
passes through (`start < tick && end > tick`, so a note that merely touches the tick is left
intact rather than becoming a zero-length half). The same predicate is its `set_enabled`
condition, so the entry greys out instead of running a no-op. It is the template for any future
position-dependent command.

**Menu overrides.** `EditFunctionInfo::menu_path` is declarative: it is set once at registration
and there is no setter, so a command cannot be re-placed at runtime. An empty path means
`kDefaultMenuPath` (`Tools/Editing`). Otherwise it is one or two non-empty slash-separated
segments: one segment places the commands directly in that top-level menu, two segments create a
real submenu (the projector opens it in `draw_section`), so the second segment is the submenu
name. The first segment must be one of `kKnownSections` - the seven top-level names the bar
emits (`File`, `Edit`, `View`, `Project`, `Tools`, `Options`, `Help`).

A malformed path - three or more segments, an empty segment (`Tools/`, `/Tools`, `Edit//Notes`),
or an unknown section - is a registration fault: it is reported once at startup and the command
is left out of every menu. It stays registered and invocable by id, so nothing becomes
unreachable; it is simply not listed. It is never silently rendered in the default group. The bar
hard-codes no command group: every section asks `has_section()` / `draw_section()` and appends
the projected block, which is what makes an override a relocation rather than a duplicate.

Current overrides:

| Command id | `menu_path` | Rationale |
|---|---|---|
| `selection.filter_channels` | `Edit/Channels` | operating on a selection's channels is an Edit concern |
| `note.slice_at_tick` | `Edit/Notes` | playhead-relative note edit |
| the parameterised transform passes | `""` (`Tools/Editing`) | bulk transforms stay in the default bucket |

**Parameter contract.** Declaring any `EditParam` (or using the `EditFunctionArgs` overload of
`set_action`) sets `needs_params`. The no-argument `invoke()` then refuses the entry and logs,
so a parameterised command can never bypass its dialog. `params_of()` and `default_args()`
supply the dialog's contents; the dialog knows no command by name.

**Invocation contract.** `invoke()` returns `false` and logs when the id is unknown, when the
entry's `enabled` predicate rejects the current controller state, or when parameters are
required but absent. Commands stay responsible for their own transaction shape
(`begin_action` → create mutators → mutate → `tx.selection().remap()` → `commit()`), because
undo labels and selection remapping are command-specific.

**Plugins are deliberately outside this registry.** Plugin entries keep their own
`Tools → Plugins` path through `MainWindow::run_plugin`; a future change will require plugins
to declare their own menu path. Do not route plugins through `EditFunctionMenu`.

**Known deferred marker.** The only remaining `// refactor (todo):` in the tree is the inert
search field in `ui/dialogs/main_menu_bar.cpp`: filtering is not wired up, and the field is
read-only so it cannot capture keystrokes the editor needs. When search lands it should
consume this registry rather than re-scanning the menu tree.
