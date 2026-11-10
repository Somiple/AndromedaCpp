#pragma once

#include <functional>
#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "ui/dialogs/dialog.h"

namespace andromeda::app {

/// Builds the single instance of one dialog type.
using DialogFactory = std::function<std::unique_ptr<Dialog>()>;

/// One registry entry: how to build the dialog and what to run when it confirms.
struct DialogRegistration {
    std::string name;                             ///< stable key; also the instance identity
    DialogFactory factory;                        ///< builds the instance on first open
    std::function<void(Dialog&)> on_confirmed{};  ///< runs after the dialog closed by confirming
};

/// Owns one instance per dialog type plus the registration order.
/// refactor: replaces DialogManager, whose `std::any` argument queue could not express
/// "this dialog, this payload" and which rebuilt an instance on every open, so a dialog could
/// never keep its own state between openings.
class DialogRegistry {
public:
    DialogRegistry() = default;

    /// Registers @p entry. A duplicate name is logged and ignored so the first registration wins.
    void add(DialogRegistration entry);

    /// Convenience registration for a dialog type.
    /// @tparam T dialog type; must derive from Dialog and be constructible with no arguments.
    /// @param name registry key.
    /// @param on_confirmed optional handler run after @p T confirmed; receives the instance.
    template <typename T>
    void add(std::string name, std::function<void(T&)> on_confirmed = {}) {
        DialogRegistration entry;
        entry.name = std::move(name);
        entry.factory = [] { return std::make_unique<T>(); };
        if (on_confirmed) {
            entry.on_confirmed = [handler = std::move(on_confirmed)](Dialog& dialog) {
                handler(static_cast<T&>(dialog));
            };
        }
        add(std::move(entry));
    }

    /// @param name registry key.
    /// @return the instance, or nullptr when the name is unknown or never opened.
    [[nodiscard]] Dialog* find(std::string_view name) const;

    /// Opens @p name with @p payload, constructing the instance on first use.
    /// @param name registry key; an unknown name is logged and ignored.
    /// @param payload argument handed to the dialog's on_open().
    /// @return the instance, or nullptr when the name is unknown.
    Dialog* open(std::string_view name, const DialogPayload& payload = DialogPayload{ });

    /// Requests @p name to close.
    /// @param name registry key; unknown or never-opened names are ignored.
    void close(std::string_view name);

    /// Fires the registered on_confirmed handler for @p name.
    /// @param name registry key; a name without a handler is ignored.
    void notify_confirmed(std::string_view name);

    /// @return true while any registered dialog reports is_opened().
    [[nodiscard]] bool is_any_open() const;

    /// @return the currently open dialog, or nullptr.
    [[nodiscard]] Dialog* active() const;

    /// @return the registry key of the currently open dialog, or an empty view.
    [[nodiscard]] std::string_view active_name() const;

    /// @return registration order; used by the presenter and by tests.
    [[nodiscard]] const std::vector<std::string>& order() const noexcept { return order_; }

    /// @param name registry key.
    /// @return true when @p name is registered.
    [[nodiscard]] bool contains(std::string_view name) const;

private:
    struct Entry {
        DialogFactory factory;                        ///< builds the instance
        std::function<void(Dialog&)> on_confirmed{};  ///< confirm handler, may be empty
        std::unique_ptr<Dialog> instance{};           ///< created on first open
        bool registered = false;                      ///< on_register() already ran
    };

    /// @param name registry key.
    /// @return the entry, or nullptr when unknown.
    [[nodiscard]] Entry* entry_for(std::string_view name);

    /// @param entry entry to materialise.
    /// @return the instance, creating it and running on_register() on first use.
    Dialog* materialise(Entry& entry);

    std::vector<std::string> order_{};
    std::map<std::string, Entry, std::less<>> entries_{};
};

} // namespace andromeda::app
