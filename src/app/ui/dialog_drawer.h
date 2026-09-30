#pragma once

#include <functional>
#include <memory>
#include <vector>

#include "app/ui/dialog.h"
#include "app/ui/dialog_manager.h"

namespace andromeda::app {

class ImageResources;

class DialogDrawer {
public:
    void init(std::shared_ptr<DialogManager> manager) { dialog_manager_ = std::move(manager); }

    [[nodiscard]] const std::shared_ptr<DialogManager>& manager() const { return dialog_manager_; }

    void draw_all_dialogs(const ImageResources& image_resources);

    std::function<void()> on_terminate_app;

private:
    struct DrawResult {
        MaybeDlgAction action;
        MaybeDlgAction btn_action;
    };

    DrawResult draw_dialog(Dialog& dialog, const ImageResources& image_resources);
    MaybeDlgAction draw_action_buttons(Dialog& dialog, const DialogActionButtons& buttons);

    void handle_dialog_actions(std::vector<DialogAction> actions);

    std::shared_ptr<DialogManager> dialog_manager_;
};

}
