#pragma once

#include <string_view>

#include "ui/dialogs/dialog_registry.h"

namespace andromeda::app {

class ImageResources;

/// Draws the registry's open dialog and routes its outcome.
/// refactor: replaces DialogDrawer, which walked a per-frame vector of `DialogAction` variants and
/// needed the manager to resolve them. The dialog now closes itself and the registry's
/// on_confirmed handler is the only place a command's result is applied.
class DialogPresenter {
public:
    struct Context {
        DialogRegistry& registry;      ///< owns the dialog instances
        const ImageResources& images;  ///< shared handle handed to every dialog
    };

    /// @param context registry and image set; both must outlive the presenter.
    explicit DialogPresenter(Context context);

    /// Installs the shared image handle on every instantiated dialog.
    /// Cheap and idempotent, so calling it once per frame before draw() is fine.
    void install_resources();

    /// Draws the active dialog, if any, and fires its confirm handler when it closed by
    /// confirming. Safe to call every frame.
    void draw();

private:
    Context context_;
};

} // namespace andromeda::app
