#include "editor/plugins/plugin_error_dialog.h"

#include <format>

#include <imgui.h>

#include "core/debugger.h"

namespace andromeda::editor {

void PluginErrorDialog::on_open(const app::DialogPayload& payload) {
    const auto* options = payload.get_if<PluginErrorOptions>();
    if (options == nullptr) {
        util::Debugger::log_error(
            "PluginErrorDialog opened without a PluginErrorOptions payload");
        return;
    }

    plugin_name_ = options->plugin_name;
    error_msg_ = options->error_msg;
}

void PluginErrorDialog::draw_content() {
    ImGui::TextWrapped("%s",
                       std::format("Plugin '{}' failed to run. See error below.", plugin_name_)
                           .c_str());
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", error_msg_.c_str());
    ImGui::PopStyleColor();
    ImGui::TextWrapped("Fix the error(s) mentioned above, then reload plugins and try again.");
}

}
