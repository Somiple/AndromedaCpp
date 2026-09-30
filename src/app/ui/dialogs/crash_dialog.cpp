#include "app/ui/dialogs/crash_dialog.h"

#include <format>
#include <optional>

#include <imgui.h>

#include "util/crash_handler.h"
#include "util/debugger.h"
#include "util/util.h"

namespace andromeda::app {

using util::Debugger;

namespace {

#ifdef EMBEDDED_CLIENT_KEY
constexpr const char* API_KEY = EMBEDDED_CLIENT_KEY;
#else
constexpr const char* API_KEY = "";
#endif

constexpr const char* CRASH_WEBHOOK_URL =
    "https://nonconvertibly-untrue-denise.ngrok-free.dev/send";

}

std::expected<void, std::string> CrashDialog::init_dialog(DialogArgs&) {
    msg_ = util::take_last_panic().value_or("Unknown panic");
    user_crash_details_.fill('\0');
    return {};
}

MaybeDlgAction CrashDialog::draw(const ImageResources&) {
    ImGui::TextWrapped(
        "A problem has occured and Andromeda needs to shut down. Sorry for the inconvenience. A "
        "report will automatically be sent to developers.");
    ImGui::Separator();

    ImGui::TextUnformatted("Details");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", msg_.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::TextUnformatted("Optionally, specify what caused the crash in the field(s) below.");
    ImGui::TextUnformatted("Name");
    ImGui::InputText("##crash_name", user_crash_name_.data(), user_crash_name_.size());
    ImGui::TextUnformatted("How did Andromeda crash?");
    ImGui::InputTextMultiline("##crash_details", user_crash_details_.data(),
                              user_crash_details_.size());

    return std::nullopt;
}

void CrashDialog::send_report() const {
    Debugger::log_error(std::format("Details of crash: \n{}", msg_));

    const std::string name = user_crash_name_.data();
    const std::string details = user_crash_details_.data();

    const auto result = util::send_discord_webhook_crash_message(
        CRASH_WEBHOOK_URL, msg_, API_KEY,
        name.empty() ? std::nullopt : std::optional<std::string>(name),
        details.empty() ? std::nullopt : std::optional<std::string>(details));

    if (!result) {
        Debugger::log_error(std::format("Could not send the crash report: {}", result.error()));
    }

    Debugger::log_error(std::format("Cause of crash (according to user): {}", details));
    Debugger::log_error("Closing Andromeda...");
}

std::optional<DialogActionButtons> CrashDialog::get_action_buttons() const {
    return DlgOk{[](Dialog& dlg) -> MaybeDlgAction {
        static_cast<const CrashDialog&>(dlg).send_report();
        return DialogTerminateApp{};
    }};
}

}
