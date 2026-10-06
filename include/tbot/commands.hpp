#pragma once

#include "tbot/config.hpp"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace tbot {

struct CommandContext {
    std::int64_t user_id{};
    std::int64_t chat_id{};
    bool private_chat{false};
};

struct CommandResponse {
    std::string text;
    bool show_menu{false};
};

struct CommandButton {
    std::string text;
    std::size_t row{};
};

using CommandHandler = std::function<CommandResponse(const CommandContext&)>;

struct Command {
    std::string name;
    std::string description;
    std::optional<CommandButton> button;
    CommandHandler handler;
    bool requires_2fa{false};
    bool enabled{true};
};

class CommandRegistry {
public:
    void add(Command command);

    const Command* find(std::string_view name) const;
    const Command* find_by_button_text(std::string_view text) const;
    const std::vector<Command>& all() const noexcept;

private:
    std::vector<Command> commands_;
    std::unordered_map<std::string, std::size_t> indexes_;
};

void register_builtin_commands(CommandRegistry& registry);
void register_script_commands(CommandRegistry& registry,
                              const std::vector<ScriptCommandConfig>& commands);

}  // namespace tbot
