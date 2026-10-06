#include "tbot/application.hpp"

#include <cassert>
#include <filesystem>
#include <string>
#include <vector>
#include <unistd.h>

namespace {
std::atomic_bool stopped{false};
nlohmann::json updates = nlohmann::json::array();
std::vector<std::string> responses;
std::vector<std::string> acknowledgements;
}

// In-memory Telegram transport: exercise the actual update loop without network access.
namespace tbot {
TelegramClient::TelegramClient(std::string) {}
nlohmann::json TelegramClient::get_updates(long long, int) const {
    stopped = true;
    return updates;
}
long long TelegramClient::send_message(long long, std::string_view text,
                                       const nlohmann::json&) const {
    responses.emplace_back(text);
    return responses.size();
}
void TelegramClient::answer_callback_query(std::string_view id, std::string_view) const {
    acknowledgements.emplace_back(id);
}
void TelegramClient::edit_menu(long long, long long, const nlohmann::json&) const {}
// Menu rendering is tested with the real implementation in core_tests.cpp.
nlohmann::json main_menu_markup(const CommandRegistry&, bool) {
    return {{"inline_keyboard", nlohmann::json::array()}};
}
}

int main() {
    using namespace tbot;
    using Json = nlohmann::json;
    const auto socket = "/tmp/tbot-disabled-" + std::to_string(getpid()) + ".sock";
    const Json config = {
        {"telegram", {{"token", "test-token"}, {"allowed_user_ids", {42, 43}},
            {"two_factor_users", {{{"user_id", 42},
                {"secret", "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"}}}}}},
        {"notifications", {{"chat_ids", {42}}, {"socket_path", socket}}}
    };
    int disabled_calls = 0, active_calls = 0;
    CommandRegistry registry;
    registry.add({.name = "disabled", .description = "Disabled",
        .button = CommandButton{.text = "Disabled", .row = 0},
        .handler = [&](const CommandContext&) { ++disabled_calls; return CommandResponse{}; },
        .requires_2fa = true, .enabled = false});
    registry.add({.name = "active", .description = "Active",
        .handler = [&](const CommandContext&) { ++active_calls; return CommandResponse{.text = "active"}; }});
    Application app(TbotConfig::from_json(std::filesystem::temp_directory_path() / "config.json",
                                          config, {{"commands", Json::array()}}),
                    registry, TwoFactor::Clock::now, [] { return INT64_C(20000000000); });
    assert(app.execute("/auth 353130", {42, 42, true}).show_menu);
    const auto unknown = app.execute("/unknown", {42, 42, true}).text;
    long long id = 0;
    for (const auto user : {42, 43}) {
        for (const auto* prefix : {"command:", "menu:"}) {
            ++id;
            updates.push_back({{"update_id", id}, {"callback_query", {
                {"id", std::to_string(id)}, {"from", {{"id", user}}},
                {"message", {{"chat", {{"id", user}, {"type", "private"}}}}},
                {"data", std::string(prefix) + "disabled"}}}});
        }
        for (const auto* text : {"/disabled", "Disabled"}) {
            updates.push_back({{"update_id", ++id}, {"message", {
                {"from", {{"id", user}}}, {"chat", {{"id", user}, {"type", "private"}}},
                {"text", text}}}});
        }
    }
    updates.push_back({{"update_id", ++id}, {"callback_query", {
        {"id", std::to_string(id)}, {"from", {{"id", 42}}},
        {"message", {{"chat", {{"id", 42}, {"type", "private"}}}}},
        {"data", "command:active"}}}});
    app.run(stopped);
    assert(disabled_calls == 0 && active_calls == 1);
    assert(acknowledgements.size() == 5);
    assert(responses.size() == 9);
    for (std::size_t i = 0; i < 8; ++i) assert(responses[i] == unknown);
    assert(responses.back() == "active");
}
