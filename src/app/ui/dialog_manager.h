#pragma once

#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <variant>
#include <vector>

#include "app/ui/dialog.h"

namespace andromeda::app {

using DialogFactory = std::function<std::unique_ptr<Dialog>()>;

struct DialogOpenOK {
    const char* name;
};
struct DialogOpenError {
    const char* name;
    std::string error;
};
using DialogOpenResult = std::variant<DialogOpenOK, DialogOpenError>;

struct DialogCloseOK {
    const char* name;
};
struct DialogCloseCancelled {
    const char* name;
};
struct DialogCloseError {
    const char* name;
    std::string error;
};
using DialogCloseResult = std::variant<DialogCloseOK, DialogCloseCancelled, DialogCloseError>;

class DialogManager {
public:
    void register_dialog(const char* dialog_name, DialogFactory factory);

    void open_dialog_by_name(const char* dialog_name, DialogArgs args);
    void open_dialog(std::unique_ptr<Dialog> dlg, DialogArgs args);
    void close_dialog(const char* dlg_id);

    std::unique_ptr<Dialog> take_last_closed_dialog();

    [[nodiscard]] std::vector<Dialog*> get_opened_dialogs();

    [[nodiscard]] Dialog* find_opened_dialog(const char* dlg_id);

    [[nodiscard]] bool is_any_dialog_shown() const { return opened_dialog_counter_ > 0; }
    [[nodiscard]] bool is_dialog_open(const char* dlg_id) const;

    void close_all_dialogs();

private:
    void push_open_ok(const char* name);
    void push_open_err(const char* name, std::string msg);
    void push_close_ok(const char* name);
    void push_close_err(const char* name, std::string msg);

    struct OpenedDialog {
        const char* id;
        std::unique_ptr<Dialog> dialog;
    };

    std::unordered_map<std::string, DialogFactory> dialog_registry_;
    std::vector<OpenedDialog> opened_dialogs_;

    std::deque<DialogOpenResult> dlg_open_results_;
    std::deque<DialogCloseResult> dlg_close_results_;
    std::unique_ptr<Dialog> last_closed_dialog_;

    std::size_t opened_dialog_counter_ = 0;
};

}
