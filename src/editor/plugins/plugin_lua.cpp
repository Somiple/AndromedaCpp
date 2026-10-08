#include "editor/plugins/plugin_lua.h"
#include "editor/editing/lua_note_editing.h"
#include <format>
#include <fstream>
#include <regex>
#include <sstream>
#include <utility>
#include <vector>

#include "util/debugger.h"

namespace andromeda::editor {

using util::Debugger;

PluginLua::PluginLua() : lua_(std::make_shared<sol::state>()) {
    sol::state& lua = *lua_;
    lua_->open_libraries(sol::lib::base, sol::lib::coroutine, sol::lib::string, sol::lib::math,
                         sol::lib::table, sol::lib::utf8, sol::lib::package);

    sol::protected_function require = lua["require"];
    lua.set_function("require", [require](const std::string& name) -> sol::object {
        static const std::vector<std::string> disallowed_modules = { "socket", "os", "package" };
        for (const std::string& m : disallowed_modules) {
            if (m == name) {
                throw std::runtime_error(std::format(
                    "for security reasons, usage of module '{}' is not allowed", name));
            }
        }

        sol::protected_function_result result = require(name);
        if (!result.valid()) {
            const sol::error err = result;
            throw std::runtime_error(err.what());
        }
        return result;
    });

    lua["os"] = sol::nil;
    lua["package"] = sol::nil;
    lua["socket"] = sol::nil;

    LuaNoteEditing::register_types(*lua_);
}

std::expected<void, LuaError> PluginLua::load_plugin_from_path(std::filesystem::path path) {
    if (loaded_) {
        return {};
    }

    std::ifstream file(path, std::ios::binary);
    if (!file) {
        return std::unexpected("Failed to read plugin");
    }

    std::ostringstream contents;
    contents << file.rdbuf();

    plugin_path_ = path;
    if (auto result = load_plugin_from_str(contents.str()); !result) {
        return result;
    }
    is_builtin_ = false;

    return {};
}

std::expected<void, LuaError> PluginLua::reload_plugin() {
    if (is_builtin_) {
        Debugger::log(
            std::format("Skipping {} reload because it is a builtin plugin", plugin_name));
        return {};
    }

    if (!plugin_path_) {
        return std::unexpected("This plugin's path is invalid.");
    }

    loaded_ = false;
    std::filesystem::path path = std::move(*plugin_path_);
    plugin_path_.reset();
    auto result = load_plugin_from_path(std::move(path));

    if (!result) {
        Debugger::log_error(std::format("[PluginError] (while reloading {}): \n--> {}",
                                        plugin_name, result.error()));
    }

    return result;
}

std::expected<void, LuaError> PluginLua::load_plugin_from_str(const std::string& src_code) {
    if (loaded_) {
        return {};
    }

    sol::state& lua = *lua_;

    const std::string chunk_name = 
        plugin_path_ ? "@" + plugin_path_->filename().string() : std::string("=plugin");

    sol::protected_function_result chunk =
        lua.safe_script(src_code, sol::script_pass_on_error, chunk_name);
    if (!chunk.valid()) {
        const sol::error err = chunk;
        return std::unexpected(err.what());
    }

    const sol::object returned = chunk;
    if (!returned.is<sol::table>()) {
        return std::unexpected("plugin must return its table (usually `return P`)");
    }
    const sol::table globals = returned.as<sol::table>();

    const sol::optional<std::string> name = globals["plugin_name"];
    if (!name) {
        return std::unexpected(
            "plugin requires a name to be defined, which is missing (maybe try defining "
            "P.plugin_name)");
    }

    PluginType type = PluginType::Manipluate;
    {
        const sol::optional<std::string> plugin_type_str = globals["plugin_type"];
        if (!plugin_type_str) {
            return std::unexpected("plugin_type is missing");
        }
        if (*plugin_type_str == "manipulate") {
            type = PluginType::Manipluate;
        } else if (*plugin_type_str == "generate") {
            type = PluginType::Generate;
        } else {
            type = PluginType::Manipluate;
        }
    }

    if (const sol::optional<sol::table> info = globals["plugin_info"]) {
        PluginInfo plugin_info_;
        if (const sol::optional<std::string> author = (*info)["author"]) {
            plugin_info_.author = *author;
        }
        if (const sol::optional<std::string> description = (*info)["description"]) {
            plugin_info_.description = *description;
        }
        plugin_info = std::move(plugin_info_);
    }

    const sol::optional<sol::protected_function> on_apply = globals["on_apply"];
    if (!on_apply) {
        return std::unexpected("plugin requires an on_apply function");
    }

    const sol::optional<sol::table> dialog_fields = globals["dialog_fields"];

    {
        const sol::protected_function installer = lua.load(R"(
            local P = ...
            function get_field_value(f_id)
                local fields = P.dialog_fields
                if type(fields) ~= "table" then return nil end
                for _, f in ipairs(fields) do
                    if f.id == f_id and type(f[1]) == "table" then
                        return f[1].value
                    end
                end
                return nil
            end
        )");

        const sol::protected_function_result installed = installer(globals);
        if (!installed.valid()) {
            const sol::error err = installed;
            return std::unexpected(err.what());
        }
    }

    plugin_name = *name;
    plugin_type = type;
    on_apply_fn_ = *on_apply;
    dialog_field_table_ = dialog_fields ? *dialog_fields : sol::table{};
    loaded_ = true;

    return {};
}

}
