#include "ui/dialogs/editor_info.h"

#include <format>

#include <imgui.h>

#include "ui/style/theme.h"
#include "ui/dialogs/ui_helpers.h"
#include "ui/util/image_loader.h"
#include "version.h"

namespace andromeda::app {

void EditorInfo::draw_content() {
    /* TODO: draw logo here. */
    ImGui::Separator();
    ImGui::Text(std::format("Version: {}-{}", EDITOR_VERSION, EDITOR_STAGE).c_str());
    ImGui::Text("Lead programmer: ponluxime");
    // refactor: a stray EndTabBar() used to close a tab bar this dialog never opened, which
    // asserts the first time the about box is shown.
}

} // namespace andromeda::app
