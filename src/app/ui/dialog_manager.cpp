#include "app/ui/dialog_manager.h"

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>

#include "util/debugger.h"

namespace andromeda::app {

using util::Debugger;

void DialogManager::register_dialog(const char* dialog_name, DialogFactory factory) {
    dialog_registry_.insert_or_assign(dialog_name, std::move(factory));
}

void DialogManager::open_dialog_by_name(const char* dialog_name, DialogArgs args) {
    const auto it = dialog_registry_.find(dialog_name);
    if (it == dialog_registry_.end()) {
        push_open_err(dialog_name, std::format("No such dialog: {}", dialog_name));
        return;
    }

    open_dialog(it->second(), std::move(args));
}

void DialogManager::open_dialog(std::unique_ptr<Dialog> dlg, DialogArgs args) {
    if (!dlg) {
        return;
    }

    const char* dlg_id = dlg->get_dialog_name();

    if (is_dialog_open(dlg_id)) {
        Debugger::log_warning(
            std::format("[WARNING] Dialog with ID {} is already open, will close old Dialog",
                        dlg_id));
        close_dialog(dlg_id);
    }

    if (auto result = dlg->init_dialog(args); result.has_value()) {
        opened_dialogs_.push_back(OpenedDialog{dlg_id, std::move(dlg)});
        opened_dialog_counter_ += 1;
        push_open_ok(dlg_id);
    } else {
        push_open_err(dlg_id, result.error());
    }
}

void DialogManager::close_dialog(const char* dlg_id) {
    const auto it = std::find_if(opened_dialogs_.begin(), opened_dialogs_.end(),
                                 [dlg_id](const OpenedDialog& d) {
                                     return std::string_view(d.id) == dlg_id;
                                 });
    if (it == opened_dialogs_.end()) {
        push_close_err(dlg_id, "Dialog was never open");
        return;
    }

    if (auto result = it->dialog->cleanup_dialog(); result.has_value()) {
        std::unique_ptr<Dialog> dlg = std::move(it->dialog);
        opened_dialogs_.erase(it);
        opened_dialog_counter_ -= 1;
        push_close_ok(dlg_id);
        last_closed_dialog_ = std::move(dlg);
    } else {
        push_close_err(dlg_id, result.error());
    }
}

std::unique_ptr<Dialog> DialogManager::take_last_closed_dialog() {
    return std::move(last_closed_dialog_);
}

std::vector<Dialog*> DialogManager::get_opened_dialogs() {
    std::vector<Dialog*> out;
    out.reserve(opened_dialogs_.size());
    for (const OpenedDialog& d : opened_dialogs_) {
        out.push_back(d.dialog.get());
    }
    return out;
}

Dialog* DialogManager::find_opened_dialog(const char* dlg_id) {
    for (const OpenedDialog& d : opened_dialogs_) {
        if (std::string_view(d.id) == dlg_id) {
            return d.dialog.get();
        }
    }
    return nullptr;
}

bool DialogManager::is_dialog_open(const char* dlg_id) const {
    return std::any_of(opened_dialogs_.begin(), opened_dialogs_.end(),
                       [dlg_id](const OpenedDialog& d) {
                           return std::string_view(d.id) == dlg_id;
                       });
}

void DialogManager::close_all_dialogs() {
    std::vector<const char*> to_close;
    to_close.reserve(opened_dialogs_.size());
    for (const OpenedDialog& d : opened_dialogs_) {
        to_close.push_back(d.id);
    }

    for (const char* dlg_id : to_close) {
        close_dialog(dlg_id);
    }
}

void DialogManager::push_open_ok(const char* name) {
    dlg_open_results_.push_front(DialogOpenOK{name});
}

void DialogManager::push_open_err(const char* name, std::string msg) {
    Debugger::log_error(std::format("[Dialog] {}: {}", name, msg));
    dlg_open_results_.push_front(DialogOpenError{name, std::move(msg)});
}

void DialogManager::push_close_ok(const char* name) {
    dlg_close_results_.push_front(DialogCloseOK{name});
}

void DialogManager::push_close_err(const char* name, std::string msg) {
    Debugger::log_warning(std::format("[Dialog] {}: {}", name, msg));
    dlg_close_results_.push_front(DialogCloseError{name, std::move(msg)});
}

}
