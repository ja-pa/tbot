#include "tbot/config.hpp"
#include <arpa/inet.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>
#include <stdexcept>
#include <unordered_set>
#include <utility>
#include <sys/un.h>

namespace tbot {
namespace {

nlohmann::json read_json(const std::filesystem::path& path,
                         const std::string& description) {
    if (!std::filesystem::is_regular_file(path)) {
        throw std::runtime_error(description + " is not an existing regular file: " +
                                 path.string());
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Cannot open " + description + ": " + path.string());
    }

    nlohmann::json json;
    try {
        input >> json;
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Invalid JSON in " + description);
    }
    return json;
}

bool is_valid_command_name(const std::string& name) {
    return name != "auth" && name != "lock" && name != "start" &&
           name != "help" && name != "version" && !name.empty() &&
           std::all_of(name.begin(), name.end(), [](unsigned char character) {
               return std::islower(character) || std::isdigit(character) ||
                      character == '_';
           });
}

bool is_executable(const std::filesystem::path& path) {
    using perms = std::filesystem::perms;
    const auto permissions = std::filesystem::status(path).permissions();
    return (permissions & (perms::owner_exec | perms::group_exec | perms::others_exec)) !=
           perms::none;
}

}  // namespace

CommandsConfig::CommandsConfig(std::filesystem::path source_path,
                               std::vector<ScriptCommandConfig> items)
    : source_path_(std::move(source_path)), items_(std::move(items)) {}

CommandsConfig CommandsConfig::load(const std::filesystem::path& path) {
    return from_json(path, read_json(path, "command configuration"));
}

CommandsConfig CommandsConfig::from_json(const std::filesystem::path& path,
                                         const nlohmann::json& json) {
    const auto source_path = std::filesystem::weakly_canonical(path);

    std::vector<ScriptCommandConfig> commands;
    std::unordered_set<std::string> names;

    try {
        if (!json.at("commands").is_array())
            throw std::runtime_error("commands must be an array");
        for (const auto& item : json.at("commands")) {
            ScriptCommandConfig command{
                .name = item.at("name").get<std::string>(),
                .description = item.at("description").get<std::string>(),
                .button_text = std::nullopt,
                .button_row = 0,
                .script = resolve_path(source_path, item.at("script").get<std::string>()),
                .requires_2fa = item.value("requires_2fa", false),
                .enabled = item.value("enabled", true),
            };
            if (item.contains("button")) {
                const auto row = item.at("button").value("row", nlohmann::json(0));
                if (!row.is_number_integer() || row < 0 || row > 100)
                    throw std::runtime_error("Button row must be an integer from 0 to 100");
                command.button_text = item.at("button").at("text").get<std::string>();
                command.button_row = item.at("button").value("row", 0U);
            }

            validate_command(command);
            if (!names.insert(command.name).second) {
                throw std::runtime_error("Duplicate command in configuration: " + command.name);
            }
            commands.push_back(std::move(command));
        }
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Missing or incorrectly typed command configuration field");
    }

    return CommandsConfig(source_path, std::move(commands));
}

const std::vector<ScriptCommandConfig>& CommandsConfig::items() const noexcept {
    return items_;
}

const std::filesystem::path& CommandsConfig::source_path() const noexcept {
    return source_path_;
}

std::filesystem::path CommandsConfig::resolve_path(
    const std::filesystem::path& config_file,
    std::filesystem::path value) {
    if (value.is_relative()) {
        value = config_file.parent_path() / value;
    }
    return std::filesystem::weakly_canonical(value);
}

void CommandsConfig::validate_command(const ScriptCommandConfig& command) {
    if (!is_valid_command_name(command.name)) {
        throw std::runtime_error(
            "Invalid command name '" + command.name +
            "'; only lowercase letters, digits and underscores are allowed");
    }
    if (command.description.empty()) {
        throw std::runtime_error("Empty command description: " + command.name);
    }
    if (command.button_text && command.button_text->empty()) {
        throw std::runtime_error("Empty command button text: " + command.name);
    }
    const auto script = command.script.string();
    if (!std::all_of(script.begin(), script.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-';
        }))
        throw std::runtime_error("Script path contains disallowed characters");
    if (!std::filesystem::is_regular_file(command.script)) {
        throw std::runtime_error("Script for command '" + command.name +
                                 "' is not an existing regular file: " +
                                 command.script.string());
    }
    if (!is_executable(command.script)) {
        throw std::runtime_error("Script for command '" + command.name +
                                 "' is not executable: " + command.script.string());
    }
}

TbotConfig::TbotConfig(TelegramConfig telegram,
                       NotificationsConfig notifications,
                       CommandsConfig commands)
    : telegram_(std::move(telegram)),
      notifications_(std::move(notifications)),
      commands_(std::move(commands)) {}

TbotConfig TbotConfig::load(const std::filesystem::path& path) {
    const auto config_path = std::filesystem::weakly_canonical(path);
    const auto json = read_json(config_path, "configuration file");
    try {
        const auto commands_path = resolve_path(config_path,
            json.value("commands_file", std::string("commands.json")));
        return from_json(config_path, json, read_json(commands_path, "command configuration"));
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Incorrect configuration field type");
    }
}

TbotConfig TbotConfig::from_json(const std::filesystem::path& config_path,
                               const nlohmann::json& json,
                               const nlohmann::json& commands_json) {

    TelegramConfig telegram;
    NotificationsConfig notifications;
    std::filesystem::path commands_path;

    try {
        const auto& telegram_json = json.at("telegram");
        telegram.token = telegram_json.at("token").get<std::string>();
        telegram.help_text = telegram_json.value("help_text", telegram.help_text);
        if (telegram_json.contains("poll_timeout_seconds") &&
            (!telegram_json.at("poll_timeout_seconds").is_number_integer() ||
             telegram_json.at("poll_timeout_seconds") < 1 ||
             telegram_json.at("poll_timeout_seconds") > 50))
            throw std::runtime_error("Polling interval must be an integer from 1 to 50");
        telegram.poll_timeout_seconds = telegram_json.value("poll_timeout_seconds", 30);
        if (!telegram_json.at("allowed_user_ids").is_array())
            throw std::runtime_error("allowed_user_ids must be an array");
        for (const auto& user_id : telegram_json.at("allowed_user_ids")) {
            if (!user_id.is_number_integer() || user_id <= 0 ||
                user_id > INT64_MAX)
                throw std::runtime_error("user_id must be a positive int64 integer");
            telegram.allowed_user_ids.insert(user_id.get<std::int64_t>());
        }

        if (telegram_json.contains("two_factor_users")) {
            const auto& users = telegram_json.at("two_factor_users");
            if (!users.is_array()) throw std::runtime_error("two_factor_users must be an array");
            for (const auto& user : users) {
                if (!user.at("user_id").is_number_integer() || user.at("user_id") <= 0 ||
                    user.at("user_id") > INT64_MAX)
                    throw std::runtime_error("2FA user_id must be a positive int64 integer");
                const auto id = user.at("user_id").get<std::int64_t>();
                const auto secret = user.at("secret").get<std::string>();
                if (!telegram.allowed_user_ids.contains(id) ||
                    !telegram.two_factor_secrets.emplace(id, secret).second)
                    throw std::runtime_error("2FA user_id must be unique and included in the allowlist");
            }
            TwoFactor validation(telegram.two_factor_secrets);
        }

        if (!json.contains("notifications")) {
            throw std::runtime_error("Missing required notifications section");
        }
        const auto& notifications_json = json.at("notifications");
        if (!notifications_json.at("chat_ids").is_array())
            throw std::runtime_error("chat_ids must be an array");
        std::unordered_set<std::int64_t> seen_chat_ids;
        for (const auto& chat_id : notifications_json.at("chat_ids")) {
            if (!chat_id.is_number_integer() || chat_id > INT64_MAX)
                throw std::runtime_error("chat_id must be an int64 integer");
            const auto value = chat_id.get<std::int64_t>();
            if (value == 0) {
                throw std::runtime_error("notifications.chat_ids must not contain zero");
            }
            if (seen_chat_ids.insert(value).second) {
                notifications.chat_ids.push_back(value);
            }
        }
        notifications.socket_path = notifications_json.value(
            "socket_path", std::string("/tmp/tbot-wrt.sock"));
        if (notifications_json.contains("http")) {
            const auto& http = notifications_json.at("http");
            if (!http.is_object()) throw std::runtime_error("notifications.http must be an object");
            auto& target = notifications.http;
            target.enabled = http.value("enabled", false);
            target.listen_address = http.value("listen_address", std::string("127.0.0.1"));
            if (http.contains("port") && (!http.at("port").is_number_integer() ||
                http.at("port") < 1 || http.at("port") > 65535))
                throw std::runtime_error("HTTP port must be an integer from 1 to 65535");
            target.port = http.value("port", 8088);
            target.username = http.value("username", std::string());
            target.password = http.value("password", std::string());
        }

        commands_path = resolve_path(
            config_path, json.value("commands_file", std::string("commands.json")));
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Missing or incorrectly typed configuration field");
    }

    validate(telegram, notifications);
    return TbotConfig(std::move(telegram),
                      std::move(notifications),
                      CommandsConfig::from_json(commands_path, commands_json));
}

const TelegramConfig& TbotConfig::telegram() const noexcept {
    return telegram_;
}

const NotificationsConfig& TbotConfig::notifications() const noexcept {
    return notifications_;
}

const CommandsConfig& TbotConfig::commands() const noexcept {
    return commands_;
}

std::filesystem::path TbotConfig::resolve_path(
    const std::filesystem::path& config_file,
    std::filesystem::path value) {
    if (value.is_relative()) {
        value = config_file.parent_path() / value;
    }
    return std::filesystem::weakly_canonical(value);
}

void TbotConfig::validate(const TelegramConfig& telegram,
                          const NotificationsConfig& notifications) {
    const auto& http = notifications.http;
    in_addr address{};
    if (http.listen_address.find('\0') != std::string::npos ||
        inet_pton(AF_INET, http.listen_address.c_str(), &address) != 1 ||
        (ntohl(address.s_addr) >> 24) == 0 || (ntohl(address.s_addr) >> 24) >= 224)
        throw std::runtime_error("HTTP requires a specific unicast IPv4 address");
    const auto invalid_credential = [](const std::string& value) {
        return value.size() > 256 || value.find_first_of(std::string("\0\r\n", 3)) != std::string::npos;
    };
    if (invalid_credential(http.username) || invalid_credential(http.password) ||
        http.username.find(':') != std::string::npos)
        throw std::runtime_error("Invalid HTTP credentials (maximum 256 bytes, username without a colon)");
    if (http.enabled && (http.username.empty() || http.password.empty()))
        throw std::runtime_error("The enabled HTTP API requires a username and password");
    if (telegram.token.empty()) {
        throw std::runtime_error("telegram.token must not be empty");
    }
    if (telegram.allowed_user_ids.empty()) {
        throw std::runtime_error("telegram.allowed_user_ids must not be empty");
    }
    if (telegram.poll_timeout_seconds < 1 || telegram.poll_timeout_seconds > 50) {
        throw std::runtime_error(
            "telegram.poll_timeout_seconds must be between 1 and 50");
    }
    if (notifications.chat_ids.empty()) {
        throw std::runtime_error("notifications.chat_ids must not be empty");
    }
    if (notifications.socket_path.empty() || !notifications.socket_path.is_absolute()) {
        throw std::runtime_error("notifications.socket_path must be an absolute path");
    }
    const auto socket = notifications.socket_path.string();
    if (socket.find('\0') != std::string::npos || socket.size() >= sizeof(sockaddr_un{}.sun_path))
        throw std::runtime_error("Invalid or excessively long Unix socket path");
}

}  // namespace tbot
