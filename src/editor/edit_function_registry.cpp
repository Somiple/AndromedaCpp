#include "editor/edit_function_registry.h"

#include <format>
#include <limits>
#include <utility>

#include "core/debugger.h"

namespace andromeda::editor {

namespace {

using util::Debugger;

/// Index value used by detached builders; never a real entry index.
constexpr std::size_t kDetachedIndex = std::numeric_limits<std::size_t>::max();

} // namespace

bool EditParam::valid() const {
    if (id.empty()) {
        return false;
    }
    if (type == EditParamType::Choice && choices.empty()) {
        return false;
    }
    if (min_int && max_int && *min_int > *max_int) {
        return false;
    }
    return true;
}

EditFunctionArgs& EditFunctionArgs::set(std::string id, EditParamValue value) {
    m_values.insert_or_assign(std::move(id), std::move(value));
    return *this;
}

void EditFunctionArgs::prefill(const std::vector<EditParam>& params) {
    for (const EditParam& param : params) {
        if (param.id.empty() || m_values.contains(param.id)) {
            continue;
        }
        switch (param.type) {
        case EditParamType::Integer:
            m_values.emplace(param.id, param.default_int);
            break;
        case EditParamType::Decimal:
            m_values.emplace(param.id, param.default_decimal);
            break;
        case EditParamType::Boolean:
            m_values.emplace(param.id, param.default_bool);
            break;
        case EditParamType::Text:
            m_values.emplace(param.id, param.default_text);
            break;
        case EditParamType::Choice:
            m_values.emplace(param.id, param.choices.front());
            break;
        }
    }
}

bool EditFunctionArgs::has(std::string_view id) const {
    return m_values.contains(std::string(id));
}

int EditFunctionArgs::get_int(std::string_view id, int fallback) const {
    const auto it = m_values.find(std::string(id));
    if (it == m_values.end()) return fallback;
    if (const int* value = std::get_if<int>(&it->second)) return *value;
    if (const double* value = std::get_if<double>(&it->second)) return static_cast<int>(*value);
    if (const bool* value = std::get_if<bool>(&it->second)) return *value ? 1 : 0;
    return fallback;
}

double EditFunctionArgs::get_decimal(std::string_view id, double fallback) const {
    const auto it = m_values.find(std::string(id));
    if (it == m_values.end()) return fallback;
    if (const double* value = std::get_if<double>(&it->second)) return *value;
    if (const int* value = std::get_if<int>(&it->second)) return static_cast<double>(*value);
    return fallback;
}

bool EditFunctionArgs::get_bool(std::string_view id, bool fallback) const {
    const auto it = m_values.find(std::string(id));
    if (it == m_values.end()) return fallback;
    if (const bool* value = std::get_if<bool>(&it->second)) return *value;
    if (const int* value = std::get_if<int>(&it->second)) return *value != 0;
    return fallback;
}

std::string EditFunctionArgs::get_text(std::string_view id, std::string fallback) const {
    const auto it = m_values.find(std::string(id));
    if (it == m_values.end()) return fallback;
    if (const std::string* value = std::get_if<std::string>(&it->second)) return *value;
    return fallback;
}

std::string EditFunctionArgs::get_choice(std::string_view id, std::string fallback) const {
    return get_text(id, std::move(fallback));
}

EditFunctionBuilder::EditFunctionBuilder(EditFunctionRegistry& owner, std::size_t index) noexcept
    : m_owner(&owner), m_index(index) {}

EditFunctionBuilder& EditFunctionBuilder::set_action(
    std::function<void(EditorController&)> action) {
    if (m_owner == nullptr) return *this;
    if (EditFunctionRegistry::Entry* entry = m_owner->entry_at(m_index)) {
        entry->action = std::move(action);
    }
    return *this;
}

EditFunctionBuilder& EditFunctionBuilder::set_action(
    std::function<void(EditorController&, const EditFunctionArgs&)> action) {
    if (m_owner == nullptr) return *this;
    if (EditFunctionRegistry::Entry* entry = m_owner->entry_at(m_index)) {
        entry->action_with_args = std::move(action);
        m_owner->m_infos[m_index].needs_params = true;
    }
    return *this;
}

EditFunctionBuilder& EditFunctionBuilder::add_param(EditParam param) {
    if (m_owner == nullptr) return *this;
    if (!param.valid()) {
        Debugger::log_error(std::format("edit function '{}' declared an invalid parameter '{}'",
                                        m_owner->m_infos[m_index].id, param.id));
        return *this;
    }
    EditFunctionRegistry::Entry* entry = m_owner->entry_at(m_index);
    if (entry == nullptr) return *this;

    for (const EditParam& existing : entry->params) {
        if (existing.id == param.id) {
            Debugger::log_error(std::format(
                "edit function '{}' declared parameter '{}' twice",
                m_owner->m_infos[m_index].id, param.id));
            return *this;
        }
    }

    entry->params.push_back(std::move(param));
    m_owner->m_infos[m_index].needs_params = true;
    return *this;
}

EditFunctionBuilder& EditFunctionBuilder::set_enabled(
    std::function<bool(EditorController&)> predicate) {
    if (m_owner == nullptr) return *this;
    if (EditFunctionRegistry::Entry* entry = m_owner->entry_at(m_index)) {
        entry->enabled = std::move(predicate);
    }
    return *this;
}

EditFunctionBuilder& EditFunctionBuilder::set_shortcut_label(std::string label) {
    if (m_owner == nullptr) return *this;
    m_owner->m_infos[m_index].shortcut_label = std::move(label);
    return *this;
}

EditFunctionRegistry::EditFunctionRegistry() = default;
EditFunctionRegistry::~EditFunctionRegistry() = default;
EditFunctionRegistry::EditFunctionRegistry(EditFunctionRegistry&&) noexcept = default;
EditFunctionRegistry& EditFunctionRegistry::operator=(EditFunctionRegistry&&) noexcept = default;

EditFunctionRegistry::Entry* EditFunctionRegistry::entry_at(std::size_t index) {
    return index < m_entries.size() ? m_entries[index].get() : nullptr;
}

const EditFunctionRegistry::Entry* EditFunctionRegistry::entry_at(std::size_t index) const {
    return index < m_entries.size() ? m_entries[index].get() : nullptr;
}

EditFunctionRegistry::Entry* EditFunctionRegistry::find_entry(std::string_view id) {
    const auto it = m_index.find(std::string(id));
    if (it == m_index.end()) return nullptr;
    return entry_at(it->second);
}

EditFunctionBuilder& EditFunctionRegistry::add_function(EditFunctionInfo info) {
    if (info.id.empty()) {
        Debugger::log_error("edit function registered without an id");
        m_builders.push_back(std::make_unique<EditFunctionBuilder>(*this, kDetachedIndex));
        return *m_builders.back();
    }

    if (m_index.contains(info.id)) {
        Debugger::log_error(std::format(
            "edit function '{}' is already registered; keeping the first", info.id));
        m_builders.push_back(std::make_unique<EditFunctionBuilder>(*this, kDetachedIndex));
        return *m_builders.back();
    }

    const std::size_t index = m_entries.size();
    m_index.emplace(info.id, index);
    m_infos.push_back(std::move(info));
    m_entries.push_back(std::make_unique<Entry>());
    m_builders.push_back(std::make_unique<EditFunctionBuilder>(*this, index));
    return *m_builders.back();
}

const EditFunctionInfo* EditFunctionRegistry::find(std::string_view id) const {
    const auto it = m_index.find(std::string(id));
    if (it == m_index.end()) return nullptr;
    return &m_infos[it->second];
}

const std::vector<EditParam>& EditFunctionRegistry::params_of(std::string_view id) const {
    const Entry* entry = entry_at(m_index.contains(std::string(id)) ? m_index.at(std::string(id))
                                                                   : kDetachedIndex);
    if (entry == nullptr) return m_empty_params;
    return entry->params;
}

EditFunctionArgs EditFunctionRegistry::default_args(std::string_view id) const {
    EditFunctionArgs args;
    args.prefill(params_of(id));
    return args;
}

bool EditFunctionRegistry::is_enabled(std::string_view id, EditorController& controller) const {
    const auto it = m_index.find(std::string(id));
    if (it == m_index.end()) return false;

    const Entry* entry = entry_at(it->second);
    if (entry == nullptr || !entry->enabled) return true;
    return entry->enabled(controller);
}

bool EditFunctionRegistry::invoke(std::string_view id, EditorController& controller) {
    const EditFunctionInfo* info = find(id);
    if (info == nullptr) {
        Debugger::log_error(std::format("edit function '{}' is not registered", id));
        return false;
    }
    if (!is_enabled(id, controller)) {
        Debugger::log_warning(std::format("edit function '{}' is disabled in the current state", id));
        return false;
    }
    if (info->needs_params) {
        Debugger::log_error(std::format(
            "edit function '{}' requires parameters; use the parameter dialog", id));
        return false;
    }

    Entry* entry = find_entry(id);
    if (entry == nullptr || !entry->action) {
        Debugger::log_error(std::format("edit function '{}' has no action", id));
        return false;
    }

    entry->action(controller);
    return true;
}

bool EditFunctionRegistry::invoke(std::string_view id, const EditFunctionArgs& args,
                                  EditorController& controller) {
    const EditFunctionInfo* info = find(id);
    if (info == nullptr) {
        Debugger::log_error(std::format("edit function '{}' is not registered", id));
        return false;
    }
    if (!is_enabled(id, controller)) {
        Debugger::log_warning(std::format("edit function '{}' is disabled in the current state", id));
        return false;
    }

    Entry* entry = find_entry(id);
    if (entry == nullptr) {
        Debugger::log_error(std::format("edit function '{}' has no action", id));
        return false;
    }

    if (entry->action_with_args) {
        entry->action_with_args(controller, args);
        return true;
    }
    if (entry->action) {
        entry->action(controller);
        return true;
    }

    Debugger::log_error(std::format("edit function '{}' has no action", id));
    return false;
}

void EditFunctionRegistry::clear() {
    m_index.clear();
    m_infos.clear();
    m_entries.clear();
    m_builders.clear();
}

} // namespace andromeda::editor
