#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <sol/sol.hpp>

namespace andromeda::editor {

enum class PluginType { Manipluate, Generate };

struct PluginInfo {
    std::optional<std::string> author;
    std::optional<std::string> description;
};

using LuaError = std::string;

class PluginLua {
public:
    PluginLua();

    std::expected<void, LuaError> load_plugin_from_path(std::filesystem::path path);
    std::expected<void, LuaError> load_plugin_from_str(const std::string& src_code);
    std::expected<void, LuaError> reload_plugin();

    [[nodiscard]] const std::shared_ptr<sol::state>& lua() const { return lua_; }
    [[nodiscard]] const sol::protected_function& on_apply_fn() const { return on_apply_fn_; }
    [[nodiscard]] const sol::table& dialog_field_table() const { return dialog_field_table_; }
    [[nodiscard]] bool has_dialog_fields() const { return dialog_field_table_.valid(); }

    std::string plugin_name = "unnamed plugin";
    PluginType plugin_type = PluginType::Manipluate;
    std::optional<PluginInfo> plugin_info;

private:
    std::optional<std::filesystem::path> plugin_path_;

    // lua_ must be declared before the sol refs so it is destroyed last
    std::shared_ptr<sol::state> lua_;
    sol::protected_function on_apply_fn_;
    sol::table dialog_field_table_;

    bool loaded_ = false;
    bool is_builtin_ = true;
};

}
