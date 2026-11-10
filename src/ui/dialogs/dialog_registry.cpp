#include "ui/dialogs/dialog_registry.h"

#include <format>
#include <utility>

#include "core/debugger.h"

namespace andromeda::app {

void DialogRegistry::add(DialogRegistration entry) {
    if (entry.name.empty()) {
        util::Debugger::log_error("[DialogRegistry] refused a registration with an empty name");
        return;
    }

    if (entries_.contains(entry.name)) {
        util::Debugger::log_warning(
            std::format("[DialogRegistry] '{}' is already registered; keeping the first",
                        entry.name));
        return;
    }

    order_.push_back(entry.name);
    Entry stored;
    stored.factory = std::move(entry.factory);
    stored.on_confirmed = std::move(entry.on_confirmed);
    entries_.emplace(entry.name, std::move(stored));
}

DialogRegistry::Entry* DialogRegistry::entry_for(std::string_view name) {
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : &it->second;
}

Dialog* DialogRegistry::materialise(Entry& entry) {
    if (entry.instance == nullptr) {
        if (!entry.factory) {
            util::Debugger::log_error("[DialogRegistry] a registration has no factory");
            return nullptr;
        }
        entry.instance = entry.factory();
        if (entry.instance == nullptr) {
            util::Debugger::log_error("[DialogRegistry] a factory returned no dialog");
            return nullptr;
        }
    }

    if (!entry.registered) {
        entry.registered = true;
        entry.instance->on_register();
    }
    return entry.instance.get();
}

Dialog* DialogRegistry::find(std::string_view name) const {
    const auto it = entries_.find(name);
    return it == entries_.end() ? nullptr : it->second.instance.get();
}

Dialog* DialogRegistry::open(std::string_view name, const DialogPayload& payload) {
    Entry* entry = entry_for(name);
    if (entry == nullptr) {
        // the old manager pushed an error onto a queue nobody drained; a log line is the honest
        // version of the same report, and it names the entry that is missing
        util::Debugger::log_error(
            std::format("[DialogRegistry] cannot open unregistered dialog '{}'", name));
        return nullptr;
    }

    Dialog* dialog = materialise(*entry);
    if (dialog == nullptr) {
        return nullptr;
    }
    dialog->open(payload);
    return dialog;
}

void DialogRegistry::close(std::string_view name) {
    Entry* entry = entry_for(name);
    if (entry == nullptr || entry->instance == nullptr) {
        return;
    }
    entry->instance->close();
}

void DialogRegistry::notify_confirmed(std::string_view name) {
    Entry* entry = entry_for(name);
    if (entry == nullptr || entry->instance == nullptr || !entry->on_confirmed) {
        return;
    }
    entry->on_confirmed(*entry->instance);
}

bool DialogRegistry::is_any_open() const {
    return active() != nullptr;
}

Dialog* DialogRegistry::active() const {
    // registration order keeps this deterministic when two dialogs are somehow open at once
    for (const std::string& name : order_) {
        const auto it = entries_.find(name);
        if (it == entries_.end() || it->second.instance == nullptr) {
            continue;
        }
        // refactor: wants_open() rather than is_opened(); a dialog that was just asked to open is
        // not on screen yet, and the presenter is the only thing that can put it there
        if (it->second.instance->wants_open()) {
            return it->second.instance.get();
        }
    }
    return nullptr;
}

std::string_view DialogRegistry::active_name() const {
    for (const std::string& name : order_) {
        const auto it = entries_.find(name);
        if (it == entries_.end() || it->second.instance == nullptr) {
            continue;
        }
        if (it->second.instance->wants_open()) {
            return name;
        }
    }
    return {};
}

bool DialogRegistry::contains(std::string_view name) const {
    return entries_.contains(name);
}

} // namespace andromeda::app
