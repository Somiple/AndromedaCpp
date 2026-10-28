#pragma once

// ---------------------------------------------------------------------------
// Layer: editor
// Dependency direction:  core -> midi -> editor -> audio -> ui
// This header is deliberately free of ImGui, GLFW, GL and `ui/` includes so an
// editor command can be registered and invoked headlessly. The menu projection
// lives in `ui/app/edit_function_menu.*`.
// See docs/ARCHITECTURE.md section 13.
// ---------------------------------------------------------------------------

#include <cstddef>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace andromeda::editor {

class EditorController;

/// Value type a declared edit-function parameter holds.
enum class EditParamType { Integer, Decimal, Text, Boolean, Choice };

/// One declarable input of an edit function. The generic parameters dialog renders
/// exactly these fields; the registry never needs to know a concrete command.
struct EditParam {
    std::string id;              ///< key used by EditFunctionArgs
    std::string label;           ///< row label shown by the generic dialog
    EditParamType type = EditParamType::Integer;
    int default_int = 0;
    double default_decimal = 0.0;
    bool default_bool = false;
    std::string default_text;
    std::vector<std::string> choices;  ///< Choice only; must be non-empty
    std::optional<int> min_int;        ///< Integer only; optional lower clamp
    std::optional<int> max_int;        ///< Integer only; optional upper clamp

    /// @return false when the id is empty, a Choice has no choices, or min_int > max_int.
    [[nodiscard]] bool valid() const;
};

using EditParamValue = std::variant<int, double, bool, std::string>;

/// Type-erased value bag handed to a parameterised action.
class EditFunctionArgs {
public:
    EditFunctionArgs() = default;

    /// Stores a value under `id`, replacing any previous value.
    /// @return *this, so a call site can be written inline.
    EditFunctionArgs& set(std::string id, EditParamValue value);

    /// Fills every declared parameter that is not already present with its default.
    /// @param params declared parameter list of the target function.
    void prefill(const std::vector<EditParam>& params);

    [[nodiscard]] bool has(std::string_view id) const;

    [[nodiscard]] int get_int(std::string_view id, int fallback = 0) const;
    [[nodiscard]] double get_decimal(std::string_view id, double fallback = 0.0) const;
    [[nodiscard]] bool get_bool(std::string_view id, bool fallback = false) const;
    [[nodiscard]] std::string get_text(std::string_view id, std::string fallback = {}) const;
    [[nodiscard]] std::string get_choice(std::string_view id, std::string fallback = {}) const;

    [[nodiscard]] const std::unordered_map<std::string, EditParamValue>& values() const {
        return m_values;
    }

private:
    std::unordered_map<std::string, EditParamValue> m_values;
};

/// Everything the menu, the dialog and the docs need to know about a command.
struct EditFunctionInfo {
    std::string id;              ///< stable dotted id, e.g. "note.transpose"
    std::string label;           ///< menu label
    std::string menu_path;       ///< empty == default group; else "TopLevel/Sub"
    std::string shortcut_label;  ///< display only, e.g. "Ctrl+T"
    bool needs_params = false;   ///< true => must be invoked through the parameters dialog
};

class EditFunctionRegistry;

/// Fluent handle returned by EditFunctionRegistry::add_function(). Does not own the entry.
/// A detached handle (owner == nullptr) makes every call a no-op, which keeps registration
/// safe to write even when the entry was rejected.
class EditFunctionBuilder {
public:
    EditFunctionBuilder(EditFunctionRegistry& owner, std::size_t index) noexcept;
    EditFunctionBuilder(const EditFunctionBuilder&) = delete;
    EditFunctionBuilder& operator=(const EditFunctionBuilder&) = delete;

    /// Sets a command that needs no input.
    EditFunctionBuilder& set_action(std::function<void(EditorController&)> action);

    /// Sets a command that reads declared parameters. Implies needs_params = true.
    EditFunctionBuilder& set_action(
        std::function<void(EditorController&, const EditFunctionArgs&)> action);

    /// Declares one input. Implies needs_params = true.
    EditFunctionBuilder& add_param(EditParam param);

    /// Predicate deciding whether the command can run in the current state.
    EditFunctionBuilder& set_enabled(std::function<bool(EditorController&)> predicate);

    /// Display-only accelerator hint shown by the menu.
    EditFunctionBuilder& set_shortcut_label(std::string label);

private:
    EditFunctionRegistry* m_owner;  ///< nullptr == detached no-op handle
    std::size_t m_index;
};

/// Runtime registry of editor commands. Adding a command is one registration block:
/// @code
/// auto& function_builder = registry.add_function({ "note.example", "Example", "" });
/// function_builder.set_action([](EditorController& controller) { /* ... */ });
/// @endcode
class EditFunctionRegistry {
public:
    EditFunctionRegistry();
    ~EditFunctionRegistry();
    EditFunctionRegistry(EditFunctionRegistry&&) noexcept;
    EditFunctionRegistry& operator=(EditFunctionRegistry&&) noexcept;
    EditFunctionRegistry(const EditFunctionRegistry&) = delete;
    EditFunctionRegistry& operator=(const EditFunctionRegistry&) = delete;

    /// Registers a command.
    /// @param info descriptor; empty or duplicate ids are rejected and logged.
    /// @return a stable builder handle for the new (or rejected) entry.
    EditFunctionBuilder& add_function(EditFunctionInfo info);

    /// @return every registered descriptor, in registration order.
    [[nodiscard]] const std::vector<EditFunctionInfo>& functions() const { return m_infos; }

    /// @return the descriptor for `id`, or nullptr when it is not registered.
    [[nodiscard]] const EditFunctionInfo* find(std::string_view id) const;

    /// @return the declared parameters of `id`; empty when unknown or unparameterised.
    [[nodiscard]] const std::vector<EditParam>& params_of(std::string_view id) const;

    /// @return a value bag pre-filled with every declared default of `id`.
    [[nodiscard]] EditFunctionArgs default_args(std::string_view id) const;

    /// @return true when the entry has no predicate or its predicate accepts the controller.
    [[nodiscard]] bool is_enabled(std::string_view id, EditorController& controller) const;

    /// Runs a command that takes no input.
    /// @return false when the id is unknown, disabled, or requires parameters.
    bool invoke(std::string_view id, EditorController& controller);

    /// Runs a command with collected values; falls back to the no-argument action when the
    /// entry was registered without parameters.
    /// @return false when the id is unknown or disabled.
    bool invoke(std::string_view id, const EditFunctionArgs& args, EditorController& controller);

    void clear();

private:
    friend class EditFunctionBuilder;

    /// Everything about an entry except its descriptor, which lives in m_infos at the same index.
    struct Entry {
        std::vector<EditParam> params;
        std::function<void(EditorController&)> action;
        std::function<void(EditorController&, const EditFunctionArgs&)> action_with_args;
        std::function<bool(EditorController&)> enabled;
    };

    [[nodiscard]] Entry* entry_at(std::size_t index);
    [[nodiscard]] const Entry* entry_at(std::size_t index) const;
    [[nodiscard]] Entry* find_entry(std::string_view id);

    std::vector<EditFunctionInfo> m_infos;              ///< parallel to m_entries
    std::vector<std::unique_ptr<Entry>> m_entries;
    std::vector<std::unique_ptr<EditFunctionBuilder>> m_builders;
    std::unordered_map<std::string, std::size_t> m_index;
    std::vector<EditParam> m_empty_params;
};

} // namespace andromeda::editor
