#pragma once

#include "tbot/commands.hpp"
#include "tbot/config.hpp"
#include "tbot/telegram_client.hpp"

#include <atomic>
#include <cstdint>
#include <string>
#include <string_view>

namespace tbot {

class Application {
public:
    Application(TbotConfig config, CommandRegistry commands,
                TwoFactor::SteadyNow steady = TwoFactor::Clock::now,
                TwoFactor::UnixNow unix_now = {});

    void run(const std::atomic_bool& stop_requested);
    CommandResponse execute(std::string_view input, const CommandContext& context = {});
    nlohmann::json menu(const CommandContext& context) const;
    bool is_authorized(std::int64_t user_id) const;

private:
    void refresh_menus(bool all = false);
    void send_response(const CommandContext& context, const CommandResponse& response);
    std::unordered_map<std::int64_t, long long> unlocked_menus_;
    void handle_update(const nlohmann::json& update);

    TbotConfig config_;
    CommandRegistry commands_;
    TelegramClient telegram_;
    TwoFactor two_factor_;
    TwoFactor::SteadyNow steady_;
    std::unordered_map<std::int64_t, TwoFactor::Clock::time_point> awaiting_code_;
    long long next_update_id_{0};
};

}  // namespace tbot
