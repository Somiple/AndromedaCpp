#pragma once

#include <any>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace andromeda::app {

class ImageResources;

namespace dialog_flags {
inline constexpr std::uint16_t DIALOG_NO_COLLAPSABLE = 0x1;
inline constexpr std::uint16_t DIALOG_NO_RESIZABLE = 0x2;
}

namespace dialog_names {
inline constexpr const char* DIALOG_NAME_SIMPLE = "SimpleDialog";
inline constexpr const char* DIALOG_NAME_EF_STRETCH = "EFStretchDialog";
inline constexpr const char* DIALOG_NAME_EF_CHOP = "EFChopDialog";
inline constexpr const char* DIALOG_NAME_EF_GLUE = "EFGlueDialog";
inline constexpr const char* DIALOG_NAME_EF_SET_CHANNEL = "EFSetChannelDialog";
inline constexpr const char* DIALOG_NAME_EDITOR_SETTINGS = "EditorSettings";
inline constexpr const char* DIALOG_NAME_PROJECT_SETTINGS = "ProjectSettings";
inline constexpr const char* DIALOG_NAME_INSERT_META = "InsertMeta";
inline constexpr const char* DIALOG_NAME_EDITOR_MANUAL = "EditorManual";
inline constexpr const char* DIALOG_NAME_EDITOR_INFO = "EditorInfo";
inline constexpr const char* DIALOG_NAME_PLUGIN_DIALOG = "LuaPluginDialog";
inline constexpr const char* DIALOG_NAME_PLUGIN_ERROR_DIALOG = "LuaPluginErrorDialog";
inline constexpr const char* DIALOG_NAME_FILTER_CHANNELS = "FilterChannels";
inline constexpr const char* DIALOG_NAME_CRASH = "CrashDialog";
}

using DialogArgs = std::vector<std::any>;

struct DialogOpen {
    const char* name = nullptr;
    DialogArgs args;
};

struct DialogClose {
    const char* name = nullptr;
};

struct DialogTerminateApp {};

using DialogAction = std::variant<DialogOpen, DialogClose, DialogTerminateApp>;
using MaybeDlgAction = std::optional<DialogAction>;

class Dialog;

using DlgButtonAction = std::function<MaybeDlgAction(Dialog&)>;

struct DlgYesNo {
    DlgButtonAction yes;
    DlgButtonAction no;
};
struct DlgOk {
    DlgButtonAction ok;
};
struct DlgOkCancel {
    DlgButtonAction ok;
    DlgButtonAction cancel;
};
struct DlgApplyClose {
    DlgButtonAction apply;
    DlgButtonAction close;
};

using DialogActionButtons = std::variant<DlgYesNo, DlgOk, DlgOkCancel, DlgApplyClose>;

class Dialog {
public:
    virtual ~Dialog() = default;

    virtual std::expected<void, std::string> init_dialog(DialogArgs&) { return {}; }

    virtual MaybeDlgAction draw(const ImageResources& image_resources) = 0;

    virtual std::expected<void, std::string> cleanup_dialog() { return {}; }

    [[nodiscard]] virtual const char* get_dialog_name() const = 0;
    [[nodiscard]] virtual std::string get_dialog_title() const = 0;
    [[nodiscard]] virtual std::optional<DialogActionButtons> get_action_buttons() const {
        return std::nullopt;
    }
    [[nodiscard]] virtual std::uint16_t get_flags() const {
        return dialog_flags::DIALOG_NO_COLLAPSABLE | dialog_flags::DIALOG_NO_RESIZABLE;
    }
    [[nodiscard]] bool flag_enabled(std::uint16_t flag) const { return (get_flags() & flag) != 0; }
};

inline DlgButtonAction dialog_default_close_action() {
    return [](Dialog& dlg) -> MaybeDlgAction { return DialogClose{dlg.get_dialog_name()}; };
}

}
