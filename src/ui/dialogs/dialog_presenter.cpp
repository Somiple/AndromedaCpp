#include "ui/dialogs/dialog_presenter.h"

#include <string>
#include <string_view>

#include "ui/util/image_loader.h"

namespace andromeda::app {

DialogPresenter::DialogPresenter(Context context) : context_(context) { }

void DialogPresenter::install_resources() {
    for (const std::string& name : context_.registry.order()) {
        if (Dialog* dialog = context_.registry.find(name); dialog != nullptr) {
            dialog->set_image_resources(context_.images);
        }
    }
}

void DialogPresenter::draw() {
    Dialog* dialog = context_.registry.active();
    if (dialog == nullptr) {
        return;
    }

    // the name is read before drawing because a dialog that closes clears its own open state, so
    // by the time draw() returns the registry can no longer tell which entry was on screen
    const std::string name{ context_.registry.active_name() };

    dialog->draw();

    if (name.empty() || dialog->is_opened()) {
        return;
    }

    // the dialog closed this frame. Only a confirmation runs the handler the composition root
    // registered; a cancel and the title-bar close must not apply anything.
    if (dialog->was_confirmed()) {
        dialog->consume_confirmation();
        context_.registry.notify_confirmed(name);
    }
}

} // namespace andromeda::app
