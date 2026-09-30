#include "editor/plugins/plugin_error_dialog.h"

#include <format>

#include <imgui.h>

namespace andromeda::editor {

std::expected<void, std::string> PluginErrorDialog::init_dialog(app::DialogArgs& args) {
    if (args.size() < 2) {
        return std::unexpected("Plugin error dialog needs a plugin name and a message.");
    }

    const auto* name = std::any_cast<std::string>(&args[0]);
    const auto* msg = std::any_cast<std::string>(&args[1]);
    if (!name || !msg) {
        return std::unexpected("Plugin error dialog was given the wrong argument types.");
    }

    plugin_name_ = *name;
    error_msg_ = *msg;
    return {};
}

app::MaybeDlgAction PluginErrorDialog::draw(const app::ImageResources&) {
    ImGui::TextWrapped("%s",
                       std::format("Plugin '{}' failed to run. See error below.", plugin_name_)
                           .c_str());
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", error_msg_.c_str());
    ImGui::PopStyleColor();
    ImGui::TextWrapped("Fix the error(s) mentioned above, then reload plugins and try again.");
    return std::nullopt;
}

}
