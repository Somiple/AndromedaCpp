#pragma once

#include <functional>

#include "editor/editing.h"
#include "editor/history/change.h"
#include "editor/project/project_manager.h"

namespace andromeda::editor::history {

// what the executor mutates
struct EditContext {
    ProjectManager* project = nullptr;
    SharedSelectedNotes* selection = nullptr;
    std::uint16_t* active_track = nullptr;
};

// The only code that is allowed to apply a Change to the project. Transactions use it for their
// live edits, undo/redo use it to replay, so both go through exactly the same path.
class ActionExecutor {
public:
    using Observer = std::function<void(const ChangeEvent&)>;

    explicit ActionExecutor(EditContext context) : ctx_(context) {}

    void set_observer(Observer observer) { observer_ = std::move(observer); }

    [[nodiscard]] const EditContext& context() const { return ctx_; }

    // Applies one change and rewrites it into its inverse. Validates everything first, so a
    // `false` return means nothing was touched.
    [[nodiscard]] bool apply(Change& change);

    // redo direction: changes in recorded order
    [[nodiscard]] bool execute(Action& action) { return run(action, false); }
    // undo direction: changes in reverse order
    [[nodiscard]] bool execute_reversed(Action& action) { return run(action, true); }

    void notify(const ChangeEvent& event) const {
        if (observer_) observer_(event);
    }

private:
    // all-or-nothing: if change N fails, changes 0..N-1 are flipped back
    [[nodiscard]] bool run(Action& action, bool reversed);

    [[nodiscard]] bool apply_one(TrackPresenceChange& c);
    [[nodiscard]] bool apply_one(TrackSwapChange& c);
    [[nodiscard]] bool apply_one(TrackMoveChange& c);
    [[nodiscard]] bool apply_one(NotePropertyChange& c);
    [[nodiscard]] bool apply_one(NotePresenceChange& c);
    [[nodiscard]] bool apply_one(SelectionSwapChange& c);
    [[nodiscard]] bool apply_one(ActiveTrackChange& c);

    EditContext ctx_;
    Observer observer_;
};

}