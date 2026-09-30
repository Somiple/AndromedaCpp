#pragma once

#include <array>
#include <string>

#include "app/ui/dialog.h"

namespace andromeda::app {

class CrashDialog final : public Dialog {
public:
    std::expected<void, std::string> init_dialog(DialogArgs& args) override;

    MaybeDlgAction draw(const ImageResources& images) override;

    [[nodiscard]] const char* get_dialog_name() const override {
        return dialog_names::DIALOG_NAME_CRASH;
    }
    [[nodiscard]] std::string get_dialog_title() const override { return "Catastrophic Error"; }
    [[nodiscard]] std::optional<DialogActionButtons> get_action_buttons() const override;
    [[nodiscard]] std::uint16_t get_flags() const override {
        return dialog_flags::DIALOG_NO_COLLAPSABLE;
    }

private:
    void send_report() const;

    std::string msg_;
    std::array<char, 128> user_crash_name_{};
    std::array<char, 1024> user_crash_details_{};
};

}
