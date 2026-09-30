#include "util/util.h"

#include <curl/curl.h>
#include <nlohmann/json.hpp>

#include <string>

#include "version.h"

namespace andromeda::util {

namespace {

std::size_t discard_write(char*, std::size_t size, std::size_t nmemb, void*) {
    return size * nmemb;
}

}

std::expected<void, std::string> send_discord_webhook_crash_message(
    std::string_view webhook_url,
    std::string_view content,
    std::string_view api_key,
    const std::optional<std::string>& reporter_name,
    const std::optional<std::string>& report_details) {

    const nlohmann::json payload = {
        {"embeds", nlohmann::json::array({
            {
                {"title", "Crash Report"},
                {"description", std::string("**Version:** ") + EDITOR_VERSION + "-" + EDITOR_STAGE},
                {"color", 0xFF0000},
                {"fields", nlohmann::json::array({
                    {
                        {"name", "Reporter name"},
                        {"value", reporter_name.value_or("Anonymous")},
                        {"inline", false}
                    },
                    {
                        {"name", "Cause of crash"},
                        {"value", report_details.value_or("No further details given.")},
                        {"inline", false}
                    },
                    {
                        {"name", "Crash details"},
                        {"value", "```" + std::string(content) + "```"},
                        {"inline", false}
                    }
                })}
            }
        })}
    };

    const std::string body = payload.dump();
    const std::string url(webhook_url);
    const std::string api_key_header = "x-api-key: " + std::string(api_key);

    CURL* curl = curl_easy_init();
    if (curl == nullptr) {
        return std::unexpected("failed to initialise curl");
    }

    curl_slist* headers = nullptr;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, api_key_header.c_str());

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_POST, 1L);
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, body.c_str());
    curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.size()));
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, discard_write);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 30L);

    const CURLcode res = curl_easy_perform(curl);

    long status = 0;
    if (res == CURLE_OK) {
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &status);
    }

    curl_slist_free_all(headers);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK) {
        return std::unexpected(std::string(curl_easy_strerror(res)));
    }

    if (status >= 400) {
        return std::unexpected("HTTP status " + std::to_string(status));
    }

    return {};
}

}
