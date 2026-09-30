#include "editor/plugins.h"

#include <algorithm>
#include <cctype>
#include <format>
#include <fstream>
#include <sstream>
#include <utility>

#include "util/debugger.h"

namespace andromeda::editor {

using util::Debugger;

namespace {

bool is_lua_file(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    return ext == ".lua";
}

}

PluginLoader::PluginLoader(std::filesystem::path plugins_path, std::filesystem::path builtin_path)
    : plugins_path_(std::move(plugins_path)), builtin_path_(std::move(builtin_path)) {
    for (const char* file_name : BUILTIN_PLUGIN_NAMES) {
        const std::filesystem::path path = builtin_path_ / std::format("{}.lua", file_name);

        std::ifstream file(path, std::ios::binary);
        if (!file) {
            Debugger::log_warning(
                std::format("[PluginWarning] built-in plugin {} is missing", path.string()));
            continue;
        }

        std::ostringstream contents;
        contents << file.rdbuf();
        (void)push_plugin_raw_str(file_name, contents.str());
    }
}

std::expected<void, LuaError> PluginLoader::reload_plugins() {
    for (auto& plugin : manip_plugins) {
        if (auto result = plugin->reload_plugin(); !result) {
            return result;
        }
    }

    for (auto& plugin : gen_plugins) {
        if (auto result = plugin->reload_plugin(); !result) {
            return result;
        }
    }

    return {};
}

std::expected<void, std::string> PluginLoader::load_all_plugins() {
    return load_plugins(plugins_path_);
}

std::expected<void, std::string> PluginLoader::load_plugins(const std::filesystem::path& dir) {
    std::error_code ec;
    if (!std::filesystem::exists(dir, ec)) {
        return std::unexpected(std::format("plugin directory {} does not exist", dir.string()));
    }

    for (const auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (ec) {
            return std::unexpected(ec.message());
        }

        const std::filesystem::path path = entry.path();
        if (entry.is_directory()) {
            if (auto result = load_plugins(path); !result) {
                return result;
            }
        } else if (is_lua_file(path)) {
            if (auto result = push_plugin(path); !result) {
                return result;
            }
        }
    }

    return {};
}

std::expected<void, std::string> PluginLoader::push_plugin(
    const std::filesystem::path& plugin_path) {
    const std::string plugin_file_name = plugin_path.filename().string();

    auto plugin = std::make_shared<PluginLua>();
    if (auto result = plugin->load_plugin_from_path(plugin_path); result) {
        push_inner(std::move(plugin));
    } else {
        Debugger::log_error(
            std::format("[PluginError] (in {}): \n--> {}", plugin_file_name, result.error()));
    }

    return {};
}

std::expected<void, std::string> PluginLoader::push_plugin_raw_str(const std::string& plugin_name,
                                                                   const std::string& plugin_str) {
    auto plugin = std::make_shared<PluginLua>();
    if (auto result = plugin->load_plugin_from_str(plugin_str); result) {
        push_inner(std::move(plugin));
    } else {
        Debugger::log_error(
            std::format("[PluginError] (in {}): \n--> {}", plugin_name, result.error()));
    }

    return {};
}

void PluginLoader::push_inner(std::shared_ptr<PluginLua> plugin) {
    switch (plugin->plugin_type) {
    case PluginType::Manipluate:
        manip_plugins.push_back(std::move(plugin));
        break;
    case PluginType::Generate:
        gen_plugins.push_back(std::move(plugin));
        break;
    }
}

}
