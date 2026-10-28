// refactor: new translation unit. MainWindow::show_dialog_with_args and
// process_closed_dialogs moved here unchanged; the window now only forwards.

#include "ui/app/dialog_coordinator.h"

#include <format>
#include <memory>
#include <utility>

#include "core/debugger.h"
#include "ui/dialogs/dialog_manager.h"
#include "ui/dialogs/edit_function_params_dialog.h"
#include "ui/dialogs/simple_dialog.h"

namespace andromeda::app {

DialogCoordinator::DialogCoordinator(DialogCoordinatorContext context)
    : m_context(context) {}

void DialogCoordinator::open(const char* name, DialogArgs args) {
    // was MainWindow::show_dialog_with_args: one dialog at a time, newest wins.
    m_context.manager.close_all_dialogs();
    m_context.manager.open_dialog_by_name(name, std::move(args));
}

void DialogCoordinator::process_closed() {
    // was MainWindow::process_closed_dialogs: only an OK'd SimpleDialog carries an action.
    std::unique_ptr<Dialog> closed = m_context.manager.take_last_closed_dialog();
    if (!closed) {
        return;
    }

    // refactor: the generic edit-function parameters dialog is checked first. it is not a
    // SimpleDialog, so the more specific cast has to win or the collected values are lost.
    if (auto* params_dialog = dynamic_cast<EditFunctionParamsDialog*>(closed.get())) {
        if (params_dialog->ok_clicked && m_context.handle_edit_function_args) {
            m_context.handle_edit_function_args(params_dialog->function_id(),
                                               params_dialog->values());
        }
        return;
    }

    auto* simple = dynamic_cast<SimpleDialog*>(closed.get());
    if (simple == nullptr || !simple->ok_clicked) {
        return;
    }

    if (m_context.handle_confirmed_id && m_context.handle_confirmed_id(simple->id)) {
        return;
    }

    util::Debugger::log_warning(
        std::format("Don't know what to do with Simple Dialog {}", simple->id));
}

} // namespace andromeda::app
