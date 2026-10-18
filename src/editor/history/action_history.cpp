#include "editor/history/action_history.h"

#include <algorithm>
#include <utility>

#include "util/debugger.h"

namespace andromeda::editor::history {

using util::Debugger;

bool ActionHistory::undo(ActionExecutor& executor) {
    if (busy()) {
        Debugger::log_warning("Cannot undo while a transaction is open or an undo is running");
        return false;
    }
    if (cursor_ == 0) {
        Debugger::log("Nothing to undo");
        return false;
    }

    executing_ = true;
    const bool ok = executor.execute_reversed(actions_[cursor_ - 1]);
    executing_ = false;

    if (ok) {
        --cursor_;
    }
    return ok;
}

bool ActionHistory::redo(ActionExecutor& executor) {
    if (busy()) {
        Debugger::log_warning("Cannot redo while a transaction is open or an undo is running");
        return false;
    }
    if (cursor_ >= actions_.size()) {
        Debugger::log("Nothing to redo");
        return false;
    }

    executing_ = true;
    const bool ok = executor.execute(actions_[cursor_]);
    executing_ = false;

    if (ok) {
        ++cursor_;
    }
    return ok;
}

std::optional<std::string_view> ActionHistory::undo_label() const {
    if (cursor_ == 0) {
        return std::nullopt;
    }
    return std::string_view(actions_[cursor_ - 1].label);
}

std::optional<std::string_view> ActionHistory::redo_label() const {
    if (cursor_ >= actions_.size()) {
        return std::nullopt;
    }
    return std::string_view(actions_[cursor_].label);
}

void ActionHistory::clear() {
    actions_.clear();
    cursor_ = 0;
    bytes_ = 0;
}

void ActionHistory::push(Action action) {
    // a new action ends the redo branch
    while (actions_.size() > cursor_) {
        bytes_ -= std::min(bytes_, actions_.back().approx_bytes);
        actions_.pop_back();
    }

    std::size_t bytes = 0;
    for (Change& change : action.changes) {
        bytes += history::approx_bytes(change);
    }
    action.approx_bytes = bytes;
    bytes_ += bytes;

    actions_.push_back(std::move(action));
    cursor_ = actions_.size();

    enforce_limits();
}

void ActionHistory::enforce_limits() {
    // always keep the newest action, even if it alone is over budget
    while (actions_.size() > 1 &&
           (actions_.size() > limits_.max_actions || bytes_ > limits_.max_bytes)) {
        bytes_ -= std::min(bytes_, actions_.front().approx_bytes);
        actions_.pop_front();
        --cursor_;
    }
}

}