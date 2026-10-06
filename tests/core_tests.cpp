#include "tbot/application.hpp"
#include "tbot/commands.hpp"
#include "tbot/config.hpp"
#include "tbot/notification_service.hpp"
#include "tbot/script_runner.hpp"
#include "tbot/telegram_client.hpp"

#include <nlohmann/json.hpp>

#include <cassert>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

namespace {

void make_executable(const std::filesystem::path& path) {
    using permissions = std::filesystem::perms;
    std::filesystem::permissions(
        path,
        permissions::owner_read | permissions::owner_write | permissions::owner_exec |
            permissions::group_read | permissions::group_exec |
            permissions::others_read | permissions::others_exec,
        std::filesystem::perm_options::replace);
}

}  // namespace

int main() {
    const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const auto temp = std::filesystem::temp_directory_path() /
                      ("tbot-wrt-tests-" + std::to_string(suffix));
    std::filesystem::create_directories(temp / "scripts");

    const auto test_script = temp / "scripts" / "test.sh";
    {
        std::ofstream script(test_script);
        script << "#!/bin/sh\n"
                  "test \"$#\" -eq 0\n"
                  "printf 'test script output\\n'\n";
    }
    make_executable(test_script);

    const auto failing_script = temp / "scripts" / "failure.sh";
    {
        std::ofstream script(failing_script);
        script << "#!/bin/sh\n"
                  "printf 'test error\\n' >&2\n"
                  "exit 7\n";
    }
    make_executable(failing_script);

    const auto long_output_script = temp / "scripts" / "long-output.sh";
    {
        std::ofstream script(long_output_script);
        script << "#!/bin/sh\n"
                  "i=0\n"
                  "while [ \"$i\" -lt 4000 ]; do\n"
                  "    printf x\n"
                  "    i=$((i + 1))\n"
                  "done\n";
    }
    make_executable(long_output_script);

    const auto commands_path = temp / "commands-test.json";
    {
        std::ofstream commands(commands_path);
        commands << R"({
            "commands": [
                {
                    "name": "status",
                    "description": "Test status",
                    "button": {"text": "📊 Status", "row": 0},
                    "script": "scripts/test.sh"
                },
                {
                    "name": "home",
                    "enabled": true,
                    "description": "Test presence",
                    "button": {"text": "🏠 Who is home", "row": 1},
                    "script": "scripts/test.sh"
                }
            ]
        })";
    }

    const auto config_path = temp / "config.json";
    {
        std::ofstream config(config_path);
        config << R"({
            "telegram": {
                "token": "test-token",
                "allowed_user_ids": [42],
                "poll_timeout_seconds": 30
            },
            "notifications": {
                "chat_ids": [100, -200, 100],
                "socket_path": "/tmp/tbot-test.sock"
            },
            "commands_file": "commands-test.json"
        })";
    }

    auto config = tbot::TbotConfig::load(config_path);
    assert(config.commands().source_path() == std::filesystem::weakly_canonical(commands_path));
    assert(config.commands().items().size() == 2);
    assert(config.commands().items()[0].enabled);
    assert(config.commands().items()[1].enabled);
    nlohmann::json experimental = {{"commands", {{{"name", "experimental"},
        {"description", "Experimental"}, {"script", "scripts/test.sh"},
        {"button", {{"text", "Experimental"}, {"row", 99}}}, {"enabled", false}}}}};
    const auto disabled_config = tbot::CommandsConfig::from_json(commands_path, experimental);
    assert(!disabled_config.items()[0].enabled);
    for (const auto& bad : {nlohmann::json("false"), nlohmann::json(0),
                           nlohmann::json(nullptr), nlohmann::json::array(), nlohmann::json::object()}) {
        auto invalid = experimental;
        invalid["commands"][0]["enabled"] = bad;
        bool rejected = false;
        try { (void)tbot::CommandsConfig::from_json(commands_path, invalid); }
        catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    auto missing_script = experimental;
    missing_script["commands"][0]["script"] = "scripts/missing.sh";
    bool missing_disabled_script_rejected = false;
    try { (void)tbot::CommandsConfig::from_json(commands_path, missing_script); }
    catch (const std::runtime_error&) { missing_disabled_script_rejected = true; }
    assert(missing_disabled_script_rejected);
    auto not_executable = experimental;
    std::ofstream(temp / "scripts/not-executable.sh") << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(temp / "scripts/not-executable.sh", std::filesystem::perms::owner_read);
    not_executable["commands"][0]["script"] = "scripts/not-executable.sh";
    bool permissions_rejected = false;
    try { (void)tbot::CommandsConfig::from_json(commands_path, not_executable); }
    catch (const std::runtime_error&) { permissions_rejected = true; }
    assert(permissions_rejected);
    assert(config.notifications().chat_ids.size() == 2);
    assert(config.notifications().chat_ids.at(0) == 100);
    assert(config.notifications().chat_ids.at(1) == -200);
    assert(config.notifications().socket_path == "/tmp/tbot-test.sock");
    assert(config.commands().items().at(0).script ==
           std::filesystem::weakly_canonical(test_script));
    assert(tbot::run_script(test_script) == "test script output");

    const auto missing_notifications_path = temp / "config-without-notifications.json";
    {
        std::ofstream missing_notifications(missing_notifications_path);
        missing_notifications << R"({
            "telegram": {
                "token": "test-token",
                "allowed_user_ids": [42]
            },
            "commands_file": "commands-test.json"
        })";
    }

    bool missing_notifications_rejected = false;
    try {
        static_cast<void>(tbot::TbotConfig::load(missing_notifications_path));
    } catch (const std::runtime_error& error) {
        missing_notifications_rejected =
            std::string(error.what()).find("notifications") != std::string::npos;
    }
    assert(missing_notifications_rejected);

    tbot::NotificationQueue notification_queue;
    for (std::size_t i = 0; i < tbot::maximum_queued_notifications; ++i) {
        assert(!notification_queue.push("message-" + std::to_string(i)));
    }
    assert(notification_queue.push("message-overflow"));
    assert(notification_queue.size() == tbot::maximum_queued_notifications);
    std::string first_notification;
    assert(notification_queue.try_pop(first_notification));
    assert(first_notification == "message-1");

    bool failure_reported = false;
    try {
        tbot::run_script(failing_script);
    } catch (const std::runtime_error& error) {
        const std::string message = error.what();
        failure_reported = message.find("code 7") != std::string::npos &&
                           message.find("test error") != std::string::npos;
    }
    assert(failure_reported);

    const auto long_output = tbot::run_script(long_output_script);
    assert(long_output.starts_with(std::string(3800, 'x')));
    assert(long_output.ends_with("… (output truncated)"));

    tbot::CommandRegistry commands;
    tbot::register_builtin_commands(commands);
    tbot::register_script_commands(commands, config.commands().items());

    commands.add({
        .name = "test",
        .description = "Test C++ command",
        .handler = [](const tbot::CommandContext& context) {
            return tbot::CommandResponse{
                .text = "user=" + std::to_string(context.user_id),
            };
        },
    });

    const auto active_count = commands.all().size();
    tbot::register_script_commands(commands, disabled_config.items());
    assert(commands.all().size() == active_count + 1);
    assert(!commands.all().back().enabled);
    assert(commands.find("experimental") == nullptr);
    assert(commands.find_by_button_text("Experimental") == nullptr);
    bool duplicate_rejected = false;
    try { tbot::register_script_commands(commands, disabled_config.items()); }
    catch (const std::invalid_argument&) { duplicate_rejected = true; }
    assert(duplicate_rejected);
    int disabled_calls = 0;
    commands.add({.name = "disabled", .description = "Disabled",
        .button = tbot::CommandButton{.text = "Disabled", .row = 99},
        .handler = [&](const tbot::CommandContext&) {
            ++disabled_calls; return tbot::CommandResponse{.text = "unexpected"};
        }, .enabled = false});
    assert(tbot::main_menu_markup(commands, true).dump().find("command:disabled") == std::string::npos);
    const auto menu = tbot::main_menu_markup(commands);
    assert(menu.contains("inline_keyboard"));
    assert(menu.at("inline_keyboard").is_array());
    assert(menu.at("inline_keyboard").size() == 3);
    assert(menu.at("inline_keyboard").at(0).at(0).at("callback_data") ==
           "command:status");
    assert(menu.at("inline_keyboard").at(1).at(0).at("callback_data") ==
           "command:home");
    assert(!menu.contains("keyboard"));

    tbot::CommandRegistry ordered_commands;
    tbot::register_builtin_commands(ordered_commands);
    ordered_commands.add({.name = "same_row", .description = "Same row as help",
        .button = tbot::CommandButton{.text = "Same row", .row = 2},
        .handler = [](const tbot::CommandContext&) { return tbot::CommandResponse{}; }});
    ordered_commands.add({.name = "later_row", .description = "Later row",
        .button = tbot::CommandButton{.text = "Later row", .row = 5},
        .handler = [](const tbot::CommandContext&) { return tbot::CommandResponse{}; }});
    ordered_commands.add({.name = "protected", .description = "Protected command",
        .button = tbot::CommandButton{.text = "Protected", .row = 8},
        .handler = [](const tbot::CommandContext&) { return tbot::CommandResponse{}; },
        .requires_2fa = true});
    for (const bool unlocked : {false, true}) {
        const auto rows = tbot::main_menu_markup(ordered_commands, unlocked).at("inline_keyboard");
        assert(rows.size() == (unlocked ? 4 : 3));
        assert(rows.at(0).at(0).at("callback_data") == "command:same_row");
        assert(rows.at(1).at(0).at("callback_data") == "command:later_row");
        if (unlocked) {
            assert(rows.at(2).at(0).at("callback_data") == "command:protected");
        }
        assert(rows.back().size() == 1);
        assert(rows.back().at(0).at("callback_data") == "command:help");
    }
    tbot::CommandRegistry help_only;
    tbot::register_builtin_commands(help_only);
    const auto help_rows = tbot::main_menu_markup(help_only).at("inline_keyboard");
    assert(help_rows.size() == 1);
    assert(help_rows.at(0).at(0).at("callback_data") == "command:help");

    tbot::Application app(std::move(config), std::move(commands));
    assert(app.is_authorized(42));
    assert(!app.is_authorized(43));
    assert(app.execute("📊 Status", {.user_id = 42, .chat_id = 42, .private_chat = true}).text == "test script output");
    assert(app.execute("/version", {.user_id = 42, .chat_id = 42, .private_chat = true}).text == "tbot 0.1.0");
    assert(app.execute("/help", {.user_id = 42, .chat_id = 42, .private_chat = true}).text.find("Choose an option") != std::string::npos);
    assert(app.execute("/help", {.user_id = 42, .chat_id = 42, .private_chat = true}).show_menu);
    assert(app.execute("/start", {.user_id = 42, .chat_id = 42, .private_chat = true}).text.find("Welcome") != std::string::npos);
    assert(app.execute("/test", {.user_id = 42, .chat_id = 100}).text == "user=42");

    const auto unknown = app.execute("/unknown", {.user_id = 42, .chat_id = 42, .private_chat = true}).text;
    for (const auto* input : {"/disabled", "/disabled@bot", "Disabled", "/experimental", "Experimental"})
        assert(app.execute(input, {.user_id = 42, .chat_id = 42, .private_chat = true}).text == unknown);
    // Old command:disabled and menu:disabled callbacks dispatch this same /disabled input.
    assert(app.execute("/disabled", {.user_id = 42, .chat_id = -100}).text == unknown);
    assert(disabled_calls == 0);
    std::filesystem::remove_all(temp);
    std::cout << "All tests passed.\n";
    return 0;
}
