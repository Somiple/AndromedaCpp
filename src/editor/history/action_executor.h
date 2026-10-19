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

// The array of `Item`s that (track, Item) addresses; nullptr if the track does not exist.
// kGlobalTrack + MetaEvent resolves to the project's global meta events.
template <typename Item>
[[nodiscard]] std::vector<Item>* items_for(ProjectManager& project, TrackId track) {
    if constexpr (std::is_same_v<Item, midi::MetaEvent>) {
        if (track == kGlobalTrack) {
            return &project.get_project_data().global_metas;
        }
    }
    auto& tracks = *project.get_tracks();
    if (track >= tracks.size()) {
        return nullptr;
    }
    return &EventTraits<Item>::items(tracks[track]);
}

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

    // Rebuilds what is derived from the global meta events (the tempo map). apply() does this by
    // itself; Transaction calls it after its live property edits, which bypass apply().
    void refresh_global_meta() const;

private:
    // all-or-nothing: if change N fails, changes 0..N-1 are flipped back
    [[nodiscard]] bool run(Action& action, bool reversed);

    [[nodiscard]] bool apply_one(TrackPresenceChange& c);
    [[nodiscard]] bool apply_one(TrackSwapChange& c);
    [[nodiscard]] bool apply_one(TrackMoveChange& c);
    // notes, channel events and meta events all go through these two (see EventTraits)
    template <typename Item>
    [[nodiscard]] bool apply_one(PropertyChange<Item>& c);
    template <typename Item>
    [[nodiscard]] bool apply_one(PresenceChange<Item>& c);

    [[nodiscard]] bool apply_one(SelectionSwapChange& c);
    [[nodiscard]] bool apply_one(ActiveTrackChange& c);

    EditContext ctx_;
    Observer observer_;
};

}