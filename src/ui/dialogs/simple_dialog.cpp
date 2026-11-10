#include "ui/dialogs/simple_dialog.h"

#include <utility>

#include <imgui.h>

#include "core/debugger.h"

namespace andromeda::app {

void SimpleDialog::on_open(const DialogPayload& payload) {
    const auto* options = payload.get_if<SimpleDialogOptions>();
    if (options == nullptr) {
        util::Debugger::log_error("SimpleDialog opened without a SimpleDialogOptions payload");
        return;
    }

    title_ = options->title;
    msg_ = options->message;
    id = options->id;
    actions_ = options->actions;
    ok_clicked = false;
}

void SimpleDialog::draw_content() {
    ImGui::TextUnformatted(msg_.c_str());
}

} // namespace andromeda::app
