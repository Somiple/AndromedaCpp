#pragma once

// refactor: new file. dialog opening policy and closed-dialog routing used to live on
// MainWindow; they belong to this coordinator.

#include <functional>
#include <string_view>

#include "ui/dialogs/dialog.h"

namespace andromeda::app {

class DialogManager;

/**
 * @brief Narrow dependency bundle for DialogCoordinator.
 *
 * Holds a reference plus one callback: the coordinator must *cause* a confirmed-id routing
 * without owning the project actions that handle it, and never sees a MainWindow.
 */
struct DialogCoordinatorContext {
    /// Registry that owns the dialog instances and their lifetimes.
    DialogManager& manager;

    /// Routes a confirmed dialog id to the action that owns it.
    /// @return true when the id was recognised and handled.
    std::function<bool(std::string_view dialog_id)> handle_confirmed_id;
};

/**
 * @brief Owns "open this dialog" and "what happened when one closed".
 *
 * The factory table deliberately stays in MainWindow::init_dialogs(): naming the concrete
 * dialog types is window-layer knowledge, and moving it here would drag a dozen dialog
 * headers plus the editor controller into this translation unit for no separation gain.
 *
 * @note Never throws: an unrecognised id is logged, as before.
 */
class DialogCoordinator {
public:
    /**
     * @brief Stores the dependency bundle.
     * @param context borrowed reference and callback; both must outlive this object.
     */
    explicit DialogCoordinator(DialogCoordinatorContext context);

    /**
     * @brief Closes anything already open, then opens @p name with @p args.
     *        Was MainWindow::show_dialog_with_args.
     * @param name registered dialog name (see dialog_names).
     * @param args type-erased arguments for the dialog's init_dialog().
     */
    void open(const char* name, DialogArgs args = {});

    /**
     * @brief Routes the most recently closed dialog, if any, to its owner.
     *        Was MainWindow::process_closed_dialogs.
     */
    void process_closed();

private:
    DialogCoordinatorContext m_context;   ///< borrowed registry + routing callback
};

} // namespace andromeda::app
