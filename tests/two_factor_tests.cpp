#include "tbot/application.hpp"
#include "tbot/two_factor.hpp"
extern "C" {
#include "tiny2fa.h"
}
#include <cassert>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>

using namespace tbot;
using namespace std::chrono_literals;
const std::string secret = "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ";
const std::string other_secret = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";

std::string code_at(const std::string& key, std::int64_t time) {
    std::ostringstream out;
    out << std::setw(6) << std::setfill('0')
        << t2_get_key(reinterpret_cast<const uint8_t*>(key.c_str()), time);
    return out.str();
}

int main() {
    auto now = TwoFactor::Clock::time_point{} + 1h;
    std::int64_t unix_time = 59;
    const auto steady = [&] { return now; };
    const auto wall = [&] { return unix_time; };
    TwoFactor auth({{42, secret}, {43, other_secret}}, steady, wall);
    // RFC 6238 SHA-1 vectors, truncated to six digits.
    assert(code_at(secret, 59) == "287082");
    assert(code_at(secret, 1111111109) == "081804");
    assert(code_at(secret, 20000000000LL) == "353130");
    assert(!auth.authenticated(42));
    assert(!auth.verify(99, "287082"));
    assert(auth.verify(42, "287082"));
    assert(auth.authenticated(42));
    assert(!auth.authenticated(43));
    assert(!auth.verify(43, "287082"));
    assert(!auth.verify(42, "287082"));
    now += 299s;
    unix_time = 20000000000LL; // wall-clock changes do not extend the session
    assert(auth.authenticated(42));
    now += 1s;
    assert(!auth.authenticated(42));
    assert(auth.verify(42, "353130"));
    auth.lock(42);
    assert(!auth.authenticated(42));
    assert(!auth.verify(42, "353130"));

    TwoFactor throttled({{42, secret}}, steady, wall);
    for (int i = 0; i < 5; ++i) assert(!throttled.verify(42, "bad"));
    assert(!throttled.verify(42, "353130"));
    throttled.lock(42);
    assert(!throttled.verify(42, "353130"));
    now += 300s;
    assert(throttled.verify(42, "353130"));

    for (int offset : {-30, 0, 30}) {
        TwoFactor window({{42, secret}}, steady, wall);
        assert(window.verify(42, code_at(secret, unix_time + offset)));
    }
    for (int offset : {-60, 60}) {
        TwoFactor window({{42, secret}}, steady, wall);
        assert(!window.verify(42, code_at(secret, unix_time + offset)));
    }
    for (const auto& bad : {"", "12345", "1234567", "12345x", " 353130", "+353130"}) {
        TwoFactor syntax({{42, secret}}, steady, wall);
        assert(!syntax.verify(42, bad));
    }
    for (const auto& bad : {std::string("short"), std::string(32, '0'), std::string(32, 'a')}) {
        bool rejected = false;
        try { TwoFactor invalid({{42, bad}}); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    bool duplicate = false;
    try { TwoFactor invalid({{42, secret}, {43, secret}}); }
    catch (const std::runtime_error&) { duplicate = true; }
    assert(duplicate);
    assert(TwoFactor::valid_secret(TwoFactor::generate_secret()));

    const auto dir = std::filesystem::temp_directory_path() /
        ("tbot-2fa-" + std::to_string(TwoFactor::Clock::now().time_since_epoch().count()));
    std::filesystem::create_directory(dir);
    std::ofstream(dir / "commands.json") << R"({"commands":[]})";
    nlohmann::json config = {
        {"telegram", {{"token", "test"}, {"allowed_user_ids", {42, 43, 44}},
            {"two_factor_users", {{{"user_id", 42}, {"secret", secret}},
                                  {{"user_id", 43}, {"secret", other_secret}}}}}},
        {"notifications", {{"chat_ids", {42}}}}
    };
    std::ofstream(dir / "config.json") << config;
    int calls = 0;
    CommandRegistry registry;
    register_builtin_commands(registry);
    registry.add({.name = "private", .description = "Private",
        .button = CommandButton{.text = "Private", .row = 8},
        .handler = [&](const CommandContext&) { ++calls; return CommandResponse{.text = "ok"}; },
        .requires_2fa = true});
    registry.add({.name = "second", .description = "Second",
        .handler = [&](const CommandContext&) { ++calls; return CommandResponse{.text = "ok"}; },
        .requires_2fa = true});
    int disabled_calls = 0;
    registry.add({.name = "disabled", .description = "Disabled",
        .button = CommandButton{.text = "Disabled", .row = 99},
        .handler = [&](const CommandContext&) { ++disabled_calls; return CommandResponse{.text = "unexpected"}; },
        .requires_2fa = true, .enabled = false});
    registry.add({.name = "disabled_public", .description = "Disabled public",
        .button = CommandButton{.text = "Disabled public", .row = 99},
        .handler = [&](const CommandContext&) { ++disabled_calls; return CommandResponse{.text = "unexpected"}; },
        .enabled = false});
    Application app(TbotConfig::load(dir / "config.json"), registry, steady, wall);
    const CommandContext user{42, 42, true}, other{43, 43, true}, group{42, -100, false};
    const auto hidden = [&](const CommandContext& ctx) {
        return app.menu(ctx).dump().find("command:private") == std::string::npos;
    };
    assert(hidden(user));
    app.execute("/private", user);
    app.execute("Private", user);
    assert(calls == 0);
    assert(!app.execute("/auth 353130", group).show_menu);
    assert(!app.execute("/auth 353130", {44, 44, true}).show_menu);
    assert(!app.execute("/auth 353130", {99, 99, true}).show_menu);
    assert(app.execute("/auth@bot 353130", user).show_menu);
    assert(!hidden(user));
    for (const auto& ctx : {user, other, group, CommandContext{44, 44, true}}) {
        assert(app.menu(ctx).dump().find("command:disabled") == std::string::npos);
        for (const auto* input : {"/disabled", "Disabled", "/disabled_public", "Disabled public"})
            assert(app.execute(input, ctx).text == app.execute("/unknown", ctx).text);
    }
    assert(disabled_calls == 0);
    assert(hidden(other));
    assert(hidden(group));
    app.execute("/private", group);
    app.execute("Private", other);
    app.execute("/private", {42, -100, true});
    assert(calls == 0);
    assert(app.execute("/private", user).text == "ok");
    assert(app.execute("Private", user).text == "ok");
    assert(app.execute("/second", user).text == "ok");
    assert(calls == 3);
    now += 299s;
    assert(app.execute("/private", user).text == "ok");
    now += 1s;
    assert(hidden(user));
    app.execute("/private", user);
    assert(calls == 4);
    unix_time += 30;
    assert(app.execute("/auth " + code_at(secret, unix_time), user).show_menu);
    app.execute("/lock", user);
    assert(hidden(user));
    app.execute("/private", user);
    assert(calls == 4);
    assert(!app.execute("/auth " + code_at(secret, unix_time), user).show_menu);
    Application restarted(TbotConfig::load(dir / "config.json"), registry, steady, wall);
    assert(restarted.menu(user).dump().find("command:private") == std::string::npos);

    // Configuration must propagate the flag and reject ambiguous/invalid secrets.
    std::ofstream(dir / "script.sh") << "#!/bin/sh\nexit 0\n";
    std::filesystem::permissions(dir / "script.sh", std::filesystem::perms::owner_all);
    nlohmann::json scripts = {{"commands", {{{"name", "protected"}, {"description", "test"},
        {"script", "script.sh"}, {"requires_2fa", true}}}}};
    std::ofstream(dir / "commands.json") << scripts;
    assert(TbotConfig::load(dir / "config.json").commands().items().at(0).requires_2fa);
    for (const auto& bad_users : {
        nlohmann::json::array({{{"user_id", 42}, {"secret", "invalid"}}}),
        nlohmann::json::array({{{"user_id", 99}, {"secret", secret}}}),
        nlohmann::json::array({{{"user_id", 42}, {"secret", secret}}, {{"user_id", 43}, {"secret", secret}}}),
        nlohmann::json::array({{{"user_id", 42}, {"secret", secret}}, {{"user_id", 42}, {"secret", other_secret}}})}) {
        config["telegram"]["two_factor_users"] = bad_users;
        std::ofstream(dir / "config.json") << config;
        bool rejected = false;
        try { TbotConfig::load(dir / "config.json"); }
        catch (const std::runtime_error& error) {
            rejected = true;
            assert(std::string(error.what()).find(secret) == std::string::npos);
        }
        assert(rejected);
    }
    std::filesystem::remove_all(dir);
}
