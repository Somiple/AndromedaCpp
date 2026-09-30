#pragma once

#include <expected>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "editor/plugins/plugin_lua.h"

namespace andromeda::editor {

inline constexpr const char* BUILTIN_PLUGIN_NAMES[] = {
    "arp_generator", "batch_edit", "flip_x", "flip_y", "humanize", "curve_time"};

class PluginLoader {
public:
    explicit PluginLoader(std::filesystem::path plugins_path,
                          std::filesystem::path builtin_path = "assets/plugins/builtin");

    std::expected<void, LuaError> reload_plugins();

    std::expected<void, std::string> load_all_plugins();
    std::expected<void, std::string> load_plugins(const std::filesystem::path& dir);

    std::vector<std::shared_ptr<PluginLua>> manip_plugins;
    std::vector<std::shared_ptr<PluginLua>> gen_plugins;

private:
    std::expected<void, std::string> push_plugin(const std::filesystem::path& plugin_path);
    std::expected<void, std::string> push_plugin_raw_str(const std::string& plugin_name,
                                                         const std::string& plugin_str);
    void push_inner(std::shared_ptr<PluginLua> plugin);

    std::filesystem::path plugins_path_;
    std::filesystem::path builtin_path_;
};

}
