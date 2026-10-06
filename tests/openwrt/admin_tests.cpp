#include "tbot/openwrt/admin.hpp"
#include "tbot/config.hpp"
#include <cassert>
#include <fstream>
#include <functional>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

using Json = nlohmann::json;
namespace fs = std::filesystem;

std::string read(const fs::path& path) {
    std::ifstream input(path);
    return {std::istreambuf_iterator<char>(input), {}};
}
void write(const fs::path& path, const std::string& value) { std::ofstream(path) << value; }
void rejects(const std::function<void()>& work) {
    bool failed = false;
    try { work(); } catch (const std::exception&) { failed = true; }
    assert(failed);
}
void private_file(const fs::path& path) {
    struct stat info{};
    assert(stat(path.c_str(), &info) == 0);
    assert((info.st_mode & 0777) == 0600);
}

int main() {
    char directory[] = "/tmp/tbot-admin-tests-XXXXXX";
    assert(mkdtemp(directory));
    const fs::path root(directory), config = root / "config.json", commands = root / "custom.json";
    write(root / "script.sh", "#!/bin/sh\nexit 0\n");
    chmod((root / "script.sh").c_str(), 0700);
    const auto secret = tbot::TwoFactor::generate_secret();
    Json original = {
        {"telegram", {{"token", "private-token"}, {"allowed_user_ids", {INT64_C(9223372036854775807)}},
                      {"two_factor_users", {{{"user_id", INT64_C(9223372036854775807)}, {"secret", secret}, {"extra", 7}}}},
                      {"unknown", "preserved"}}},
        {"notifications", {{"chat_ids", {INT64_C(-9223372036854775807)}}, {"socket_path", "/tmp/test.sock"}}},
        {"commands_file", "custom.json"}, {"future", {{"enabled", true}}}
    };
    Json original_commands = {{"future", 42}, {"commands", {{{"name", "status"}, {"description", "Stav"},
        {"script", "script.sh"}, {"custom", "keep me"}, {"button", {{"text", "Stav"}, {"row", 0}, {"future", 1}}}}}}};
    original["notifications"]["http"] = {{"enabled", true}, {"listen_address", "127.0.0.1"},
        {"port", 8088}, {"username", "device"}, {"password", "private-http-password"}, {"future", 123}};
    write(config, original.dump()); write(commands, original_commands.dump());
    tbot_wrt::AdminConfig admin(config);
    auto view = admin.load();
    assert(view.dump().find(secret) == std::string::npos);
    assert(view.dump().find("private-token") == std::string::npos);
    assert(view.dump().find("private-http-password") == std::string::npos);
    assert(view["http"]["password_present"] == true);
    assert(!view["http"].contains("password"));
    assert(view.at("allowed_user_ids").at(0) == "9223372036854775807");
    assert(view.at("chat_ids").at(0) == "-9223372036854775807");
    assert(view["commands"][0]["enabled"] == true);
    view["commands"][0]["enabled"] = false;
    view["token_action"] = "keep";
    view["commands"][0]["name"] = "renamed";
    view["poll_timeout_seconds"] = 25;
    admin.save(view);
    auto saved = Json::parse(read(config));
    auto saved_commands = Json::parse(read(commands));
    assert(saved.at("telegram").at("token") == "private-token");
    assert(saved["notifications"]["http"] == original["notifications"]["http"]);
    assert(saved.at("telegram").at("two_factor_users").at(0).at("secret") == secret);
    assert(saved.at("telegram").at("two_factor_users").at(0).at("extra") == 7);
    assert(saved.at("future") == original.at("future"));
    assert(saved.at("telegram").at("unknown") == "preserved");
    assert(saved_commands.at("future") == 42);
    assert(saved_commands.at("commands").at(0).at("custom") == "keep me");
    assert(saved_commands.at("commands").at(0).at("button").at("future") == 1);
    assert(saved_commands["commands"][0]["enabled"] == false);
    private_file(config); private_file(commands); private_file(config.string() + ".luci-backup");
    rejects([&] { admin.save(view); }); // stale revision

    auto legacy = admin.load(); legacy["token_action"] = "keep";
    legacy.erase("http"); // Old clients must preserve all HTTP settings, including the secret.
    assert(legacy["commands"][0]["enabled"] == false);
    legacy["commands"][0].erase("enabled");
    auto new_command = legacy["commands"][0];
    new_command["original_name"] = ""; new_command["name"] = "new_command";
    legacy["commands"].push_back(new_command);
    admin.save(legacy);
    assert(Json::parse(read(config))["notifications"]["http"] == original["notifications"]["http"]);
    auto compatibility = Json::parse(read(commands));
    assert(compatibility["commands"][0]["enabled"] == false);
    assert(compatibility["commands"][1]["enabled"] == true);
    auto cleanup = admin.load(); cleanup["token_action"] = "keep";
    cleanup["commands"].erase(1);
    admin.save(cleanup);
    auto http_replacement = admin.load(); http_replacement["token_action"] = "keep";
    http_replacement["http"]["password_action"] = "replace";
    http_replacement["http"]["password"] = "replacement-http-password";
    auto replaced = admin.save(http_replacement);
    assert(replaced.dump().find("replacement-http-password") == std::string::npos);
    assert(Json::parse(read(config))["notifications"]["http"]["password"] == "replacement-http-password");
    assert(Json::parse(read(config))["notifications"]["http"]["future"] == 123);
    auto request = admin.load(); request["token_action"] = "keep";
    const auto before_config = read(config), before_commands = read(commands);
    const auto invalid = [&](const std::function<void(Json&)>& change) {
        auto candidate = request; change(candidate);
        rejects([&] { admin.save(candidate); });
        assert(read(config) == before_config && read(commands) == before_commands);
    };
    for (const auto& bad : {Json("false"), Json(0), Json(nullptr), Json::array(), Json::object()})
        invalid([&](Json& value) { value["commands"][0]["enabled"] = bad; });
    invalid([](Json& value) { value["http"]["listen_address"] = "0.0.0.0"; });
    invalid([](Json& value) { value["http"]["port"] = 1.5; });
    invalid([](Json& value) { value["http"]["password_action"] = "erase"; });
    invalid([](Json& value) { value["http"]["password_action"] = "replace"; value["http"]["password"] = ""; });
    invalid([](Json& value) { value["poll_timeout_seconds"] = 51; });
    invalid([](Json& value) { value["poll_timeout_seconds"] = 1.5; });
    invalid([](Json& value) { value["allowed_user_ids"] = {"9223372036854775808"}; });
    invalid([](Json& value) { value["allowed_user_ids"] = {123}; });
    invalid([](Json& value) { value["chat_ids"] = {"0"}; });
    invalid([](Json& value) { value["commands"][0]["script"] = "missing.sh"; });
    invalid([](Json& value) { value["commands"][0]["script"] = "script.sh;id"; });
    for (const char* name : {"start", "help", "version", "auth", "lock"})
        invalid([&](Json& value) { value["commands"][0]["name"] = name; });
    invalid([](Json& value) { value["commands"][0]["button"]["row"] = -1; });
    invalid([](Json& value) { value["two_factor_users"][0]["secret_action"] = "replace"; value["two_factor_users"][0]["secret"] = "bad"; });
    invalid([](Json& value) { value["two_factor_users"].push_back(value["two_factor_users"][0]); });
    invalid([](Json& value) { value["two_factor_users"][0]["user_id"] = "1"; });

    request["commands"][0]["description"] = "Changed";
    rejects([&] { admin.save(request, [] { throw std::runtime_error("simulated write failure"); }); });
    assert(read(config) == before_config && read(commands) == before_commands);
    assert(!fs::exists(config.string() + ".luci-pending"));

    // Simulate abrupt process death between the two renames; next load must roll back.
    const auto child = fork(); assert(child >= 0);
    if (child == 0) { admin.save(request, [] { _exit(42); }); _exit(1); }
    int status; assert(waitpid(child, &status, 0) == child && WEXITSTATUS(status) == 42);
    assert(fs::exists(config.string() + ".luci-pending"));
    admin.load();
    assert(read(config) == before_config && read(commands) == before_commands);

    write(commands, before_commands + "\n");
    rejects([&] { admin.save(request); }); // external SSH change
    write(commands, before_commands);
    auto generated = tbot_wrt::AdminConfig::generate("9223372036854775807");
    assert(tbot::TwoFactor::valid_secret(generated.at("secret").get<std::string>()));
    assert(generated.at("qr").is_array() && !generated.at("qr").empty());
    assert(read(config) == before_config); // generation alone never writes
    request["token_action"] = "replace"; request["token"] = "replacement-token";
    request["two_factor_users"][0]["secret_action"] = "replace";
    request["two_factor_users"][0]["secret"] = generated.at("secret");
    admin.save(request);
    assert(Json::parse(read(config)).at("telegram").at("token") == "replacement-token");
    request = admin.load(); request["token_action"] = "keep";
    request["two_factor_users"] = Json::array();
    admin.save(request);
    assert(Json::parse(read(config)).at("telegram").at("two_factor_users").empty());

    // Script editing is scoped to assigned files under the dedicated scripts directory.
    fs::create_directory(root / "scripts");
    const auto script_a = root / "scripts/a.sh", script_b = root / "scripts/b.sh";
    const std::string source = "#!/bin/sh\necho original\n";
    write(script_a, source); write(script_b, source);
    chmod(script_a.c_str(), 0750); chmod(script_b.c_str(), 0700);
    saved_commands["commands"][0]["script"] = "scripts/a.sh";
    auto second = saved_commands["commands"][0]; second["name"] = "alias";
    auto third = second; third["name"] = "other"; third["script"] = "scripts/b.sh";
    saved_commands["commands"].push_back(second); saved_commands["commands"].push_back(third);
    write(commands, saved_commands.dump());
    request = admin.load(); request["token_action"] = "keep";
    assert(request["commands"][0]["enabled"] == false);
    auto script = admin.script("renamed", request.at("revision"));
    assert(admin.test_script("renamed", request.at("revision")).at("output") == "original\n");
    rejects([&] { admin.test_script("renamed", "stale"); });
    rejects([&] { admin.test_script("/etc/passwd", request.at("revision")); });
    write(script_a, "#!/bin/sh\necho stdout; echo stderr >&2; exit 7\n");
    auto execution = admin.test_script("renamed", request.at("revision"));
    assert(execution.at("exit_code") == 7 && execution.at("output") == "stdout\nstderr\n");
    write(script_a, "#!/bin/sh\nexit 0\n");
    assert(admin.test_script("renamed", request.at("revision")).at("output") == "");
    write(script_a, "#!/bin/sh\necho before-timeout; while :; do echo output; done\n");
    execution = admin.test_script("renamed", request.at("revision"));
    assert(execution.at("timed_out") == true && execution.at("truncated") == true);
    assert(execution.at("output").get<std::string>().size() == 65536);
    write(script_a, source);
    assert(script.at("content") == source);
    assert(admin.script("alias", request.at("revision")).at("path") == script.at("path"));
    rejects([&] { admin.script("renamed", "stale"); });
    rejects([&] { admin.script("/etc/passwd", request.at("revision")); });
    const auto marker = root / "must-not-execute";
    const std::string replacement = "#!/bin/sh\ntouch " + marker.string() + "\n";
    request["scripts"] = {{{"command", "renamed"}, {"revision", script.at("revision")}, {"content", replacement}}};
    auto script_request = request;
    script_request["scripts"][0]["content"] = "#!/bin/sh\nif then\n";
    rejects([&] { admin.save(script_request); });
    assert(read(script_a) == source);
    script_request["scripts"][0]["content"] = "#!/usr/bin/python3\nprint(1)\n";
    rejects([&] { admin.save(script_request); });
    script_request["scripts"][0]["content"] = "#!/bin/sh\n" + std::string(65536, 'x');
    rejects([&] { admin.save(script_request); });
    script_request = request; script_request["commands"][0]["script"] = "scripts/b.sh";
    rejects([&] { admin.save(script_request); });
    script_request = request; script_request["scripts"].push_back(script_request["scripts"][0]);
    script_request["scripts"][1]["command"] = "alias";
    rejects([&] { admin.save(script_request); });
    write(script_a, source + "# external edit\n");
    rejects([&] { admin.save(request); });
    write(script_a, source);
    chmod(script_a.c_str(), 0700);
    rejects([&] { admin.save(request); }); // permissions are part of the revision
    chmod(script_a.c_str(), 0750);
    const auto script_config_before = read(config), script_commands_before = read(commands);
    const auto other_script = admin.script("other", request.at("revision"));
    request["scripts"].push_back({{"command", "other"}, {"revision", other_script.at("revision")}, {"content", replacement}});
    rejects([&] { admin.save(request, {}, [] { throw std::runtime_error("simulated failure after script write"); }); });
    assert(read(script_a) == source && read(script_b) == source);
    assert(read(config) == script_config_before && read(commands) == script_commands_before);
    const auto script_child = fork(); assert(script_child >= 0);
    if (script_child == 0) { admin.save(request, {}, [] { _exit(43); }); _exit(1); }
    assert(waitpid(script_child, &status, 0) == script_child && WEXITSTATUS(status) == 43);
    admin.load();
    assert(read(script_a) == source && read(script_b) == source);
    assert(read(config) == script_config_before && read(commands) == script_commands_before);
    admin.save(request);
    assert(read(script_a) == replacement && read(script_b) == replacement && !fs::exists(marker));
    struct stat permissions{};
    assert(stat(script_a.c_str(), &permissions) == 0 && (permissions.st_mode & 07777) == 0750);
    assert(stat(script_b.c_str(), &permissions) == 0 && (permissions.st_mode & 07777) == 0700);
    fs::remove(script_a); fs::create_symlink(root / "script.sh", script_a);
    rejects([&] { admin.script("renamed", admin.load().at("revision")); });
    rejects([&] { admin.test_script("renamed", admin.load().at("revision")); });
    fs::remove(script_a); write(script_a, source); chmod(script_a.c_str(), 0750);
    const auto restricted = [&](const std::string& path) {
        auto changed = saved_commands; changed["commands"][0]["script"] = path;
        write(commands, changed.dump());
        rejects([&] { admin.script("renamed", admin.load().at("revision")); });
        rejects([&] { admin.test_script("renamed", admin.load().at("revision")); });
    };
    restricted("../script.sh"); restricted((root / "script.sh").string());
    fs::create_directory_symlink(root, root / "scripts/link");
    restricted("scripts/link/script.sh");
    restricted("scripts/missing.sh");
    write(commands, saved_commands.dump());

    // First installation ships placeholders; loading must still allow correcting them.
    original["telegram"]["two_factor_users"][0]["secret"] = "REPLACE-ME";
    write(config, original.dump());
    assert(admin.load().contains("validation_warning"));
    fs::remove_all(root);
}
