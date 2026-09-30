#include "app/ui/dialogs/simple_dialog.h"

#include <imgui.h>

namespace andromeda::app {

namespace {

// fixed rust bug: a missing or mistyped arg panicked (unwrap); it now fails the init
const std::string* arg_string(DialogArgs& args, std::size_t i) {
    if (i >= args.size()) {
        return nullptr;
    }
    return std::any_cast<std::string>(&args[i]);
}

}

std::expected<void, std::string> SimpleDialog::init_dialog(DialogArgs& args) {
    const std::string* title = arg_string(args, 0);
    const std::string* msg = arg_string(args, 1);
    const std::string* dlg_id = arg_string(args, 2);

    if (!title || !msg) {
        return std::unexpected("Simple dialog requires a title and a message.");
    }
    if (!dlg_id) {
        return std::unexpected("Simple dialog requires a unique ID.");
    }

    title_ = *title;
    msg_ = *msg;
    id = *dlg_id;
    // fixed rust bug: args[3] was read only with exactly four args
    const bool* yesno = args.size() >= 4 ? std::any_cast<bool>(&args[3]) : nullptr;
    is_yesno_ = yesno ? *yesno : true;
    ok_clicked = false;

    return {};
}

MaybeDlgAction SimpleDialog::draw(const ImageResources&) {
    ImGui::TextUnformatted(msg_.c_str());
    return std::nullopt;
}

std::optional<DialogActionButtons> SimpleDialog::get_action_buttons() const {
    if (is_yesno_) {
        return DlgYesNo{[](Dialog& dlg) -> MaybeDlgAction {
                            auto& self = static_cast<SimpleDialog&>(dlg);
                            self.ok_clicked = true;
                            return DialogClose{self.get_dialog_name()};
                        },
                        dialog_default_close_action()};
    }

    return DlgOk{dialog_default_close_action()};
}

}
