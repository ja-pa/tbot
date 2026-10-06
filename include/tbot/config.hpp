#pragma once

#include "tbot/two_factor.hpp"
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

namespace tbot {

struct ScriptCommandConfig {
    std::string name;
    std::string description;
    std::optional<std::string> button_text;
    std::size_t button_row{};
    std::filesystem::path script;
    bool requires_2fa{false};
    bool enabled{true};
};

struct TelegramConfig {
    std::string token;
    TwoFactor::Secrets two_factor_secrets;
    std::unordered_set<std::int64_t> allowed_user_ids;
    int poll_timeout_seconds{30};
};

struct NotificationHttpConfig {
    bool enabled{false};
    std::string listen_address{"127.0.0.1"};
    int port{8088};
    std::string username;
    std::string password;
};

struct NotificationsConfig {
    std::vector<std::int64_t> chat_ids;
    std::filesystem::path socket_path{"/tmp/tbot-wrt.sock"};
    NotificationHttpConfig http;
};

class CommandsConfig {
public:
    static CommandsConfig load(const std::filesystem::path& path);
    static CommandsConfig from_json(const std::filesystem::path& path,
                                    const nlohmann::json& document);

    const std::vector<ScriptCommandConfig>& items() const noexcept;
    const std::filesystem::path& source_path() const noexcept;

private:
    CommandsConfig(std::filesystem::path source_path,
                   std::vector<ScriptCommandConfig> items);

    static std::filesystem::path resolve_path(
        const std::filesystem::path& config_file,
        std::filesystem::path value);
    static void validate_command(const ScriptCommandConfig& command);

    std::filesystem::path source_path_;
    std::vector<ScriptCommandConfig> items_;
};

class TbotConfig {
public:
    static TbotConfig load(const std::filesystem::path& path);
    static TbotConfig from_json(const std::filesystem::path& path,
                               const nlohmann::json& document,
                               const nlohmann::json& commands);

    const TelegramConfig& telegram() const noexcept;
    const NotificationsConfig& notifications() const noexcept;
    const CommandsConfig& commands() const noexcept;

private:
    TbotConfig(TelegramConfig telegram,
               NotificationsConfig notifications,
               CommandsConfig commands);

    static std::filesystem::path resolve_path(
        const std::filesystem::path& config_file,
        std::filesystem::path value);
    static void validate(const TelegramConfig& telegram,
                         const NotificationsConfig& notifications);

    TelegramConfig telegram_;
    NotificationsConfig notifications_;
    CommandsConfig commands_;
};

}  // namespace tbot
