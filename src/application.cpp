#include "tbot/application.hpp"
#include "tbot/notification_service.hpp"
#include "tbot/notification_http.hpp"

#include <chrono>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace tbot {
namespace {

std::string command_name_from_input(std::string_view input,
                                    const CommandRegistry& commands) {
    if (input.starts_with('/')) {
        std::string name(input.substr(1));
        const auto separator = name.find_first_of(" @\n\r\t");
        if (separator != std::string::npos) {
            name.resize(separator);
        }
        return name;
    }

    if (const auto* command = commands.find_by_button_text(input)) {
        return command->name;
    }
    return {};
}

}  // namespace

Application::Application(TbotConfig config, CommandRegistry commands,
                         TwoFactor::SteadyNow steady, TwoFactor::UnixNow unix_now)
    : config_(std::move(config)),
      commands_(std::move(commands)),
      telegram_(config_.telegram().token),
      two_factor_(config_.telegram().two_factor_secrets, std::move(steady), std::move(unix_now)) {}

bool Application::is_authorized(std::int64_t user_id) const {
    return config_.telegram().allowed_user_ids.contains(user_id);
}

CommandResponse Application::execute(std::string_view input,
                                     const CommandContext& context) {
    if (!is_authorized(context.user_id)) return {.text = "Access denied"};
    const std::string name = command_name_from_input(input, commands_);
    const bool private_chat = context.private_chat && context.chat_id == context.user_id;
    if (name == "auth" || name == "lock") {
        if (!private_chat) return {.text = "Authentication is only available in private chats."};
        if (name == "lock") {
            two_factor_.lock(context.user_id);
            return {.text = "Access locked.", .show_menu = true};
        }
        const auto separator = input.find_first_of(" \t\r\n");
        if (separator == std::string_view::npos)
            return {.text = "Use /auth 123456 with the code from your authenticator app."};
        auto code = input.substr(separator);
        const auto first = code.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) code = {};
        else code = code.substr(first, code.find_last_not_of(" \t\r\n") - first + 1);
        if (!two_factor_.verify(context.user_id, code))
            return {.text = "Authentication failed. Check your code and 2FA settings. After five failed attempts, wait 5 minutes."};
        return {.text = "Authenticated for 5 minutes. Use /lock to end access.", .show_menu = true};
    }
    const auto* command = commands_.find(name);
    if (!command || !command->enabled || (command->requires_2fa &&
        (!private_chat || !two_factor_.authenticated(context.user_id)))) {
        return {
            .text = "Unknown command. Use /help or the menu buttons.",
        };
    }

    try {
        auto response = command->handler(context);
        if (private_chat && (name == "start" || name == "help"))
            response.text += "\n\n/auth 123456 — authenticate for 5 minutes\n/lock — lock access";
        return response;
    } catch (const std::exception& error) {
        return {
            .text = "Command /" + command->name + " failed: " + error.what(),
        };
    }
}

nlohmann::json Application::menu(const CommandContext& context) const {
    return main_menu_markup(commands_, is_authorized(context.user_id) &&
        context.private_chat && context.chat_id == context.user_id &&
        two_factor_.authenticated(context.user_id));
}

void Application::refresh_menus(bool all) {
    for (auto it = unlocked_menus_.begin(); it != unlocked_menus_.end();) {
        if (!all && two_factor_.authenticated(it->first)) { ++it; continue; }
        try {
            telegram_.edit_menu(it->first, it->second, main_menu_markup(commands_));
            it = unlocked_menus_.erase(it);
        } catch (const std::exception&) {
            // Keep the id and retry; authorization never depends on the edit succeeding.
            std::cerr << "Cannot hide the old 2FA menu; retrying later.\n";
            ++it;
        }
    }
}

void Application::send_response(const CommandContext& context, const CommandResponse& response) {
    refresh_menus();
    if (response.show_menu) {
        const auto old = unlocked_menus_.find(context.user_id);
        if (old != unlocked_menus_.end()) {
            try {
                telegram_.edit_menu(context.user_id, old->second, main_menu_markup(commands_));
                unlocked_menus_.erase(old);
            } catch (const std::exception&) {
                // Still deliver the response, but do not create another unlocked menu.
                std::cerr << "Failed to update the old 2FA menu.\n";
            }
        }
    }
    const bool old_menu_pending = unlocked_menus_.contains(context.user_id);
    const auto markup = response.show_menu
        ? (old_menu_pending ? main_menu_markup(commands_) : menu(context)) : nlohmann::json{};
    const auto id = telegram_.send_message(context.chat_id, response.text, markup);
    if (!old_menu_pending && response.show_menu && context.private_chat && context.chat_id == context.user_id &&
        two_factor_.authenticated(context.user_id)) unlocked_menus_[context.user_id] = id;
}

void Application::handle_update(const nlohmann::json& update) {
    if (update.contains("callback_query")) {
        const auto& callback = update.at("callback_query");
        const auto callback_id = callback.at("id").get<std::string>();
        const auto user_id = callback.at("from").at("id").get<std::int64_t>();
        const auto answer_callback = [&](std::string_view text = {}) {
            try {
                telegram_.answer_callback_query(callback_id, text);
            } catch (const std::exception& error) {
                std::cerr << "Warning: the callback can no longer be acknowledged: " << error.what()
                          << '\n';
            }
        };

        if (!is_authorized(user_id)) {
            std::cerr << "Rejected Telegram user_id=" << user_id << '\n';
            answer_callback("Access denied");
            return;
        }

        answer_callback();
        if (!callback.contains("message") || !callback.contains("data")) {
            return;
        }

        const auto data = callback.at("data").get<std::string>();
        const auto separator = data.find(':');
        if (separator == std::string::npos ||
            (data.substr(0, separator) != "command" && data.substr(0, separator) != "menu")) {
            return;
        }

        const auto chat_id = callback.at("message").at("chat").at("id").get<std::int64_t>();
        const CommandContext context{.user_id = user_id, .chat_id = chat_id,
            .private_chat = callback.at("message").at("chat").value("type", "") == "private"};
        const auto response = execute("/" + data.substr(separator + 1), context);
        send_response(context, response);
        return;
    }

    if (!update.contains("message")) {
        return;
    }

    const auto& message = update.at("message");
    if (!message.contains("text") || !message.contains("from") || !message.contains("chat")) {
        return;
    }

    const auto user_id = message.at("from").at("id").get<std::int64_t>();
    if (!is_authorized(user_id)) {
        std::cerr << "Rejected Telegram user_id=" << user_id << '\n';
        return;
    }

    const auto chat_id = message.at("chat").at("id").get<std::int64_t>();
    const auto input = message.at("text").get<std::string>();
    const CommandContext context{.user_id = user_id, .chat_id = chat_id,
            .private_chat = message.at("chat").value("type", "") == "private"};
    const auto response = execute(input, context);
    send_response(context, response);
}

void Application::run(const std::atomic_bool& stop_requested) {
    NotificationService notifications(config_.telegram().token,
                                      config_.notifications().chat_ids,
                                      config_.notifications().socket_path);
    NotificationHttpServer http(config_.notifications().http,
        [&notifications](std::string message) { return notifications.try_enqueue(std::move(message)); });

    std::cout << "Bot is running. Press Ctrl+C to stop.\n";

    while (!stop_requested.load()) {
        try {
            refresh_menus();
            const auto updates = telegram_.get_updates(
                next_update_id_, unlocked_menus_.empty() ? config_.telegram().poll_timeout_seconds : 1);
            for (const auto& update : updates) {
                next_update_id_ = update.at("update_id").get<long long>() + 1;
                handle_update(update);
            }
        } catch (const std::exception& error) {
            std::cerr << "Error: " << error.what() << "\nRetrying in 3 seconds.\n";
            for (int i = 0; i < 30 && !stop_requested.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        }
    }

    refresh_menus(true);
    std::cout << "Bot stopped.\n";
}

}  // namespace tbot
