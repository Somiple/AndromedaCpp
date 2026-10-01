#include "app/ui/dialog_drawer.h"

#include <format>
#include <string>
#include <utility>

#include <imgui.h>

#include "app/util/image_loader.h"

namespace andromeda::app {

using namespace dialog_flags;

void DialogDrawer::draw_all_dialogs(const ImageResources& image_resources) {
    if (!dialog_manager_) {
        return;
    }

    std::vector<DialogAction> dialog_actions;

    // collect actions first: handling one mutates the dialog list being walked
    for (Dialog* dialog : dialog_manager_->get_opened_dialogs()) {
        DrawResult result = draw_dialog(*dialog, image_resources);

        if (result.action) {
            dialog_actions.push_back(std::move(*result.action));
        }
        if (result.btn_action) {
            dialog_actions.push_back(std::move(*result.btn_action));
        }
    }

    handle_dialog_actions(std::move(dialog_actions));
}

DialogDrawer::DrawResult DialogDrawer::draw_dialog(Dialog& dialog,
                                                   const ImageResources& image_resources) {
    DrawResult result;

    const std::string window_id =
        std::format("{}###{}", dialog.get_dialog_title(), dialog.get_dialog_name());

    ImGuiWindowFlags flags = ImGuiWindowFlags_NoDocking;
    if (dialog.flag_enabled(DIALOG_NO_COLLAPSABLE)) {
        flags |= ImGuiWindowFlags_NoCollapse;
    }
    if (dialog.flag_enabled(DIALOG_NO_RESIZABLE)) {
        flags |= ImGuiWindowFlags_NoResize | ImGuiWindowFlags_AlwaysAutoResize;
    }

    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(
        ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
               viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
        ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));

    if (ImGui::Begin(window_id.c_str(), nullptr, flags)) {
        result.action = dialog.draw(image_resources);

        if (const auto buttons = dialog.get_action_buttons()) {
            result.btn_action = draw_action_buttons(dialog, *buttons);
        }
    }
    ImGui::End();

    return result;
}

MaybeDlgAction DialogDrawer::draw_action_buttons(Dialog& dialog,
                                                 const DialogActionButtons& buttons) {
    MaybeDlgAction action;

    ImGui::Separator();

    const auto button = [&](const char* label, const DlgButtonAction& callback) {
        if (ImGui::Button(label) && callback) {
            action = callback(dialog);
        }
    };

    std::visit(
        [&](const auto& b) {
            using T = std::decay_t<decltype(b)>;

            if constexpr (std::is_same_v<T, DlgYesNo>) {
                button("Yes", b.yes);
                ImGui::SameLine();
                button("No", b.no);
            } else if constexpr (std::is_same_v<T, DlgOk>) {
                button("Ok", b.ok);
            } else if constexpr (std::is_same_v<T, DlgOkCancel>) {
                button("Ok", b.ok);
                ImGui::SameLine();
                button("Cancel", b.cancel);
            } else {
                static_assert(std::is_same_v<T, DlgApplyClose>);
                button("Apply", b.apply);
                ImGui::SameLine();
                button("Close", b.close);
            }
        },
        buttons);

    return action;
}

void DialogDrawer::handle_dialog_actions(std::vector<DialogAction> actions) {
    for (DialogAction& action : actions) {
        std::visit(
            [this](auto& a) {
                using T = std::decay_t<decltype(a)>;

                if constexpr (std::is_same_v<T, DialogOpen>) {
                    dialog_manager_->open_dialog_by_name(a.name, std::move(a.args));
                } else if constexpr (std::is_same_v<T, DialogClose>) {
                    dialog_manager_->close_dialog(a.name);
                } else {
                    static_assert(std::is_same_v<T, DialogTerminateApp>);
                    if (on_terminate_app) {
                        on_terminate_app();
                    }
                }
            },
            action);
    }
}

}
