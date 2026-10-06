#include "tbot/config.hpp"

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
        throw std::runtime_error(description + " není existující běžný soubor: " +
                                 path.string());
    }

    std::ifstream input(path);
    if (!input) {
        throw std::runtime_error("Nelze otevřít " + description + ": " + path.string());
    }

    nlohmann::json json;
    try {
        input >> json;
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Neplatný JSON v " + description);
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
    return from_json(path, read_json(path, "konfigurace příkazů"));
}

CommandsConfig CommandsConfig::from_json(const std::filesystem::path& path,
                                         const nlohmann::json& json) {
    const auto source_path = std::filesystem::weakly_canonical(path);

    std::vector<ScriptCommandConfig> commands;
    std::unordered_set<std::string> names;

    try {
        if (!json.at("commands").is_array())
            throw std::runtime_error("commands musí být pole");
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
                    throw std::runtime_error("Řádek tlačítka musí být celé číslo 0 až 100");
                command.button_text = item.at("button").at("text").get<std::string>();
                command.button_row = item.at("button").value("row", 0U);
            }

            validate_command(command);
            if (!names.insert(command.name).second) {
                throw std::runtime_error("Duplicitní příkaz v konfiguraci: " + command.name);
            }
            commands.push_back(std::move(command));
        }
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Chybí nebo má chybný typ položka konfigurace příkazů");
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
            "Neplatný název příkazu '" + command.name +
            "'; povolená jsou malá písmena, číslice a podtržítko");
    }
    if (command.description.empty()) {
        throw std::runtime_error("Prázdný popis příkazu: " + command.name);
    }
    if (command.button_text && command.button_text->empty()) {
        throw std::runtime_error("Prázdný text tlačítka příkazu: " + command.name);
    }
    const auto script = command.script.string();
    if (!std::all_of(script.begin(), script.end(), [](unsigned char c) {
            return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                   (c >= '0' && c <= '9') || c == '/' || c == '.' || c == '_' || c == '-';
        }))
        throw std::runtime_error("Cesta skriptu obsahuje nepovolené znaky");
    if (!std::filesystem::is_regular_file(command.script)) {
        throw std::runtime_error("Skript příkazu '" + command.name +
                                 "' není existující běžný soubor: " +
                                 command.script.string());
    }
    if (!is_executable(command.script)) {
        throw std::runtime_error("Skript příkazu '" + command.name +
                                 "' není spustitelný: " + command.script.string());
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
    const auto json = read_json(config_path, "konfigurační soubor");
    try {
        const auto commands_path = resolve_path(config_path,
            json.value("commands_file", std::string("commands.json")));
        return from_json(config_path, json, read_json(commands_path, "konfigurace příkazů"));
    } catch (const nlohmann::json::exception&) {
        throw std::runtime_error("Chybný typ položky konfigurace");
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
        if (telegram_json.contains("poll_timeout_seconds") &&
            (!telegram_json.at("poll_timeout_seconds").is_number_integer() ||
             telegram_json.at("poll_timeout_seconds") < 1 ||
             telegram_json.at("poll_timeout_seconds") > 50))
            throw std::runtime_error("Polling musí být celé číslo 1 až 50");
        telegram.poll_timeout_seconds = telegram_json.value("poll_timeout_seconds", 30);
        if (!telegram_json.at("allowed_user_ids").is_array())
            throw std::runtime_error("allowed_user_ids musí být pole");
        for (const auto& user_id : telegram_json.at("allowed_user_ids")) {
            if (!user_id.is_number_integer() || user_id <= 0 ||
                user_id > INT64_MAX)
                throw std::runtime_error("user_id musí být kladné celé číslo int64");
            telegram.allowed_user_ids.insert(user_id.get<std::int64_t>());
        }

        if (telegram_json.contains("two_factor_users")) {
            const auto& users = telegram_json.at("two_factor_users");
            if (!users.is_array()) throw std::runtime_error("two_factor_users musí být pole");
            for (const auto& user : users) {
                if (!user.at("user_id").is_number_integer() || user.at("user_id") <= 0 ||
                    user.at("user_id") > INT64_MAX)
                    throw std::runtime_error("2FA user_id musí být kladné celé číslo int64");
                const auto id = user.at("user_id").get<std::int64_t>();
                const auto secret = user.at("secret").get<std::string>();
                if (!telegram.allowed_user_ids.contains(id) ||
                    !telegram.two_factor_secrets.emplace(id, secret).second)
                    throw std::runtime_error("2FA user_id musí být unikátní a povolené v allowlistu");
            }
            TwoFactor validation(telegram.two_factor_secrets);
        }

        if (!json.contains("notifications")) {
            throw std::runtime_error("Chybí povinná sekce notifications");
        }
        const auto& notifications_json = json.at("notifications");
        if (!notifications_json.at("chat_ids").is_array())
            throw std::runtime_error("chat_ids musí být pole");
        std::unordered_set<std::int64_t> seen_chat_ids;
        for (const auto& chat_id : notifications_json.at("chat_ids")) {
            if (!chat_id.is_number_integer() || chat_id > INT64_MAX)
                throw std::runtime_error("chat_id musí být celé číslo int64");
            const auto value = chat_id.get<std::int64_t>();
            if (value == 0) {
                throw std::runtime_error("notifications.chat_ids nesmí obsahovat nulu");
            }
            if (seen_chat_ids.insert(value).second) {
                notifications.chat_ids.push_back(value);
            }
        }
        notifications.socket_path = notifications_json.value(
            "socket_path", std::string("/tmp/tbot-wrt.sock"));

        commands_path = resolve_path(
            config_path, json.value("commands_file", std::string("commands.json")));
    } catch (const nlohmann::json::exception& error) {
        throw std::runtime_error("Chybí nebo má chybný typ položka konfigurace");
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
    if (telegram.token.empty()) {
        throw std::runtime_error("telegram.token nesmí být prázdný");
    }
    if (telegram.allowed_user_ids.empty()) {
        throw std::runtime_error("telegram.allowed_user_ids nesmí být prázdné");
    }
    if (telegram.poll_timeout_seconds < 1 || telegram.poll_timeout_seconds > 50) {
        throw std::runtime_error(
            "telegram.poll_timeout_seconds musí být v rozsahu 1 až 50");
    }
    if (notifications.chat_ids.empty()) {
        throw std::runtime_error("notifications.chat_ids nesmí být prázdné");
    }
    if (notifications.socket_path.empty() || !notifications.socket_path.is_absolute()) {
        throw std::runtime_error("notifications.socket_path musí být absolutní cesta");
    }
    const auto socket = notifications.socket_path.string();
    if (socket.find('\0') != std::string::npos || socket.size() >= sizeof(sockaddr_un{}.sun_path))
        throw std::runtime_error("Neplatná nebo příliš dlouhá cesta k Unix socketu");
}

}  // namespace tbot
