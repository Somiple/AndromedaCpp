#include "app/ui/editor_info.h"

#include <format>

#include <imgui.h>

#include "app/theme.h"
#include "app/ui/ui_helpers.h"
#include "app/util/image_loader.h"
#include "version.h"

namespace andromeda::app {

MaybeDlgAction EditorInfo::draw(const ImageResources& images) {
    ImGui::BeginGroup();
    ImGui::TextUnformatted("Andromeda");
    ImGui::TextUnformatted(
        std::format("VERSION {}-{}", EDITOR_VERSION, EDITOR_STAGE).c_str());
    ImGui::EndGroup();

    ImGui::SameLine();
    vertical_separator();
    ImGui::SameLine();

    const ImageHandle logo = images.get_image_handle("logo_medium");
    if (logo.valid()) {
        ImGui::Image(static_cast<ImTextureID>(logo.texture_id), ImVec2(logo.width, logo.height));
    }

    return std::nullopt;
}

}
