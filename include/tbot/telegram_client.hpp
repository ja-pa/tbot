#pragma once

#include "tbot/commands.hpp"

#include <nlohmann/json.hpp>

#include <string>
#include <string_view>

namespace tbot {

nlohmann::json main_menu_markup(const CommandRegistry& commands, bool unlocked = false, bool show_auth = false);

class TelegramClient {
public:
    explicit TelegramClient(std::string token);

    nlohmann::json get_updates(long long offset, int timeout_seconds) const;
    long long send_message(long long chat_id,
                      std::string_view text,
                      const nlohmann::json& reply_markup = nullptr) const;
    void edit_menu(long long chat_id, long long message_id, const nlohmann::json& markup) const;
    void answer_callback_query(std::string_view callback_query_id,
                               std::string_view text = {}) const;

private:
    nlohmann::json request(std::string_view method,
                           const nlohmann::json& body,
                           int timeout_seconds) const;

    std::string api_base_;
};

}  // namespace tbot
