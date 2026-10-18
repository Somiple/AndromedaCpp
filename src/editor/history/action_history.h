#pragma once

#include <cstddef>
#include <deque>
#include <optional>
#include <string_view>

#include "editor/history/action_executor.h"
#include "editor/history/change.h"

namespace andromeda::editor::history {

class Transaction;

// Stores committed actions and a cursor. Undo/redo do the work themselves through the executor
// (so there is no "give me the action and I'll apply it later" step to call twice or forget):
//
//   [ a0 a1 a2 | a3 a4 ]      cursor_ = 3: a0..a2 are applied, a3..a4 can be redone
//
// Nothing is erased or reinserted on undo; only the cursor moves and the action's changes flip
// in place.
class ActionHistory {
public:
    struct Limits {
        std::size_t max_actions = 64;
        std::size_t max_bytes = std::size_t{1} << 30;  // 1 GiB of notes/tracks held for undo
    };

    ActionHistory() = default;
    explicit ActionHistory(Limits limits) : limits_(limits) {}

    [[nodiscard]] bool can_undo() const { return !busy() && cursor_ > 0; }
    [[nodiscard]] bool can_redo() const { return !busy() && cursor_ < actions_.size(); }

    // false if there was nothing to do or the action could not be replayed (state unchanged)
    bool undo(ActionExecutor& executor);
    bool redo(ActionExecutor& executor);

    [[nodiscard]] std::optional<std::string_view> undo_label() const;
    [[nodiscard]] std::optional<std::string_view> redo_label() const;

    void clear();

    [[nodiscard]] std::size_t size() const { return actions_.size(); }
    [[nodiscard]] std::size_t approx_bytes() const { return bytes_; }
    [[nodiscard]] const Limits& limits() const { return limits_; }

    // true while a transaction is recording or undo/redo is running
    [[nodiscard]] bool busy() const { return transaction_open_ || executing_; }

private:
    friend class Transaction;

    // only Transaction::commit() adds actions
    void push(Action action);
    void enforce_limits();

    std::deque<Action> actions_;
    std::size_t cursor_ = 0;
    std::size_t bytes_ = 0;
    Limits limits_;

    bool transaction_open_ = false;
    bool executing_ = false;
};

}