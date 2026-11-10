#include "ui/dialogs/crash_dialog.h"

#include <format>
#include <optional>

#include <imgui.h>

#include "core/crash_handler.h"
#include "core/debugger.h"
#include "core/util.h"

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

void CrashDialog::on_open(const DialogPayload&) {
    last_exception = util::take_last_panic().value_or("Unknown exception");
    details.fill('\0');
}

void CrashDialog::on_click_confirm() {
    send_report();
    Debugger::log_error("Closing Andromeda...");
}

void CrashDialog::draw_content() {
    ImGui::TextWrapped(
        "A problem has occured and Andromeda needs to shut down. Sorry for the inconvenience. A "
        "report will automatically be sent to developers.");
    ImGui::Separator();

    ImGui::TextUnformatted("Details");
    ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
    ImGui::TextWrapped("%s", last_exception.c_str());
    ImGui::PopStyleColor();
    ImGui::Separator();

    ImGui::TextUnformatted("Optionally, specify what caused the crash in the field(s) below.");
    ImGui::TextUnformatted("Name");
    ImGui::InputText("##crash_name", username.data(), username.size());
    ImGui::TextUnformatted("How did Andromeda crash?");
    ImGui::InputTextMultiline("##crash_details", details.data(), details.size());
}

void CrashDialog::send_report() const {
    Debugger::log_error(std::format("Details of crash: \n{}", last_exception));

    const std::string name = username.data();
    const std::string details = username.data();

    const auto result = util::send_discord_webhook_crash_message(
        CRASH_WEBHOOK_URL, last_exception, API_KEY,
        name.empty() ? std::nullopt : std::optional<std::string>(name),
        details.empty() ? std::nullopt : std::optional<std::string>(details));

    if (!result) {
        Debugger::log_error(std::format("Could not send the crash report: {}", result.error()));
    }

    Debugger::log_error(std::format("Cause of crash (according to user): {}", details));
}

}
