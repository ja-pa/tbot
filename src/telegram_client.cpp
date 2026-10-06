#include "tbot/telegram_client.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <memory>
#include <stdexcept>

namespace tbot {
namespace {

struct CurlGlobal {
    CurlGlobal() {
        if (curl_global_init(CURL_GLOBAL_DEFAULT) != CURLE_OK) {
            throw std::runtime_error("Failed to initialize libcurl");
        }
    }
    ~CurlGlobal() { curl_global_cleanup(); }
};

CurlGlobal curl_global;

std::size_t write_response(char* data, std::size_t size, std::size_t count, void* output) {
    const auto bytes = size * count;
    static_cast<std::string*>(output)->append(data, bytes);
    return bytes;
}

using CurlHandle = std::unique_ptr<CURL, decltype(&curl_easy_cleanup)>;
using CurlHeaders = std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>;

}  // namespace

TelegramClient::TelegramClient(std::string token)
    : api_base_("https://api.telegram.org/bot" + std::move(token) + "/") {}

nlohmann::json main_menu_markup(const CommandRegistry& commands, bool unlocked) {
    std::size_t row_count = 0;
    for (const auto& command : commands.all()) {
        if (command.name != "help" && command.enabled && command.button &&
            (!command.requires_2fa || unlocked)) {
            row_count = std::max(row_count, command.button->row + 1);
        }
    }

    nlohmann::json rows = nlohmann::json::array();
    for (std::size_t row = 0; row < row_count; ++row) {
        rows.push_back(nlohmann::json::array());
    }

    for (const auto& command : commands.all()) {
        if (command.name == "help" || !command.enabled || !command.button ||
            (command.requires_2fa && !unlocked)) {
            continue;
        }
        rows.at(command.button->row).push_back({
            {"text", command.button->text},
            {"callback_data", "command:" + command.name},
        });
    }

    rows.erase(std::remove_if(rows.begin(), rows.end(), [](const auto& row) { return row.empty(); }), rows.end());
    if (const auto* help = commands.find("help");
        help && help->button && (!help->requires_2fa || unlocked)) {
        rows.push_back(nlohmann::json::array({{
            {"text", help->button->text},
            {"callback_data", "command:help"},
        }}));
    }
    return {{"inline_keyboard", std::move(rows)}};
}

nlohmann::json TelegramClient::request(std::string_view method,
                                       const nlohmann::json& body,
                                       int timeout_seconds) const {
    CurlHandle curl(curl_easy_init(), &curl_easy_cleanup);
    if (!curl) {
        throw std::runtime_error("Failed to create HTTP client");
    }

    const std::string url = api_base_ + std::string(method);
    const std::string request_body = body.dump();
    std::string response_body;

    curl_slist* raw_headers = nullptr;
    raw_headers = curl_slist_append(raw_headers, "Content-Type: application/json");
    CurlHeaders headers(raw_headers, &curl_slist_free_all);

    curl_easy_setopt(curl.get(), CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_HTTPHEADER, headers.get());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDS, request_body.c_str());
    curl_easy_setopt(curl.get(), CURLOPT_POSTFIELDSIZE, request_body.size());
    curl_easy_setopt(curl.get(), CURLOPT_WRITEFUNCTION, write_response);
    curl_easy_setopt(curl.get(), CURLOPT_WRITEDATA, &response_body);
    curl_easy_setopt(curl.get(), CURLOPT_TIMEOUT, static_cast<long>(timeout_seconds));
    curl_easy_setopt(curl.get(), CURLOPT_USERAGENT, "tbot/0.1.0");

    const CURLcode result = curl_easy_perform(curl.get());
    if (result != CURLE_OK) {
        throw std::runtime_error("Telegram HTTP request failed: " +
                                 std::string(curl_easy_strerror(result)));
    }

    long http_status = 0;
    curl_easy_getinfo(curl.get(), CURLINFO_RESPONSE_CODE, &http_status);

    nlohmann::json response;
    try {
        response = nlohmann::json::parse(response_body);
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Telegram returned an invalid response (HTTP " +
                                 std::to_string(http_status) + ")");
    }

    if (http_status < 200 || http_status >= 300 || !response.value("ok", false)) {
        const std::string description = response.value("description", "unknown error");
        throw std::runtime_error("Telegram API rejected the request: " + description);
    }

    return response.at("result");
}

nlohmann::json TelegramClient::get_updates(long long offset, int timeout_seconds) const {
    return request("getUpdates",
                   {{"offset", offset},
                    {"timeout", timeout_seconds},
                    {"allowed_updates", nlohmann::json::array({"message", "callback_query"})}},
                   timeout_seconds + 10);
}

long long TelegramClient::send_message(long long chat_id,
                                  std::string_view text,
                                  const nlohmann::json& reply_markup) const {
    nlohmann::json body{{"chat_id", chat_id}, {"text", text}};
    if (!reply_markup.is_null()) {
        body["reply_markup"] = reply_markup;
    }
    return request("sendMessage", body, 15).at("message_id").get<long long>();
}

void TelegramClient::edit_menu(long long chat_id, long long message_id,
                               const nlohmann::json& markup) const {
    request("editMessageReplyMarkup", {{"chat_id", chat_id}, {"message_id", message_id},
                                      {"reply_markup", markup}}, 15);
}

void TelegramClient::answer_callback_query(std::string_view callback_query_id,
                                           std::string_view text) const {
    nlohmann::json body{{"callback_query_id", callback_query_id}};
    if (!text.empty()) {
        body["text"] = text;
    }
    request("answerCallbackQuery", body, 15);
}

}  // namespace tbot
