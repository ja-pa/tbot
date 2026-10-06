#include "tbot/commands.hpp"

#include "tbot/script_runner.hpp"

#include <stdexcept>
#include <utility>

namespace tbot {

void CommandRegistry::add(Command command) {
    if (command.name == "auth" || command.name == "lock") {
        throw std::invalid_argument("Vyhrazený příkaz: " + command.name);
    }
    if (command.name.empty()) {
        throw std::invalid_argument("Jméno příkazu nesmí být prázdné");
    }
    if (!command.handler) {
        throw std::invalid_argument("Příkaz musí mít handler: " + command.name);
    }
    if (indexes_.contains(command.name)) {
        throw std::invalid_argument("Duplicitní příkaz: " + command.name);
    }

    const auto index = commands_.size();
    indexes_.emplace(command.name, index);
    commands_.push_back(std::move(command));
}

const Command* CommandRegistry::find(std::string_view name) const {
    const auto found = indexes_.find(std::string(name));
    if (found == indexes_.end()) {
        return nullptr;
    }
    const auto& command = commands_.at(found->second);
    return command.enabled ? &command : nullptr;
}

const Command* CommandRegistry::find_by_button_text(std::string_view text) const {
    for (const auto& command : commands_) {
        if (command.enabled && command.button && command.button->text == text) {
            return &command;
        }
    }
    return nullptr;
}

const std::vector<Command>& CommandRegistry::all() const noexcept {
    return commands_;
}

void register_builtin_commands(CommandRegistry& registry) {
    registry.add({
        .name = "start",
        .description = "Zobrazit hlavní nabídku",
        .button = std::nullopt,
        .handler = [](const CommandContext&) {
            return CommandResponse{
                .text = "Vítej v OpenWrt Telegram botovi.\n"
                        "Vyber požadovanou akci pomocí tlačítek níže.",
                .show_menu = true,
            };
        },
    });

    registry.add({
        .name = "help",
        .description = "Zobrazit nápovědu",
        .button = CommandButton{.text = "ℹ️ Nápověda", .row = 2},
        .handler = [](const CommandContext&) {
            return CommandResponse{
                .text = "Co chceš zjistit? Vyber jednu z možností níže.\n\n"
                        "Textové příkazy zůstávají dostupné jako alternativa.",
                .show_menu = true,
            };
        },
    });

    registry.add({
        .name = "version",
        .description = "Verze aplikace",
        .button = std::nullopt,
        .handler = [](const CommandContext&) {
            return CommandResponse{.text = "tbot 0.1.0"};
        },
    });
}

void register_script_commands(CommandRegistry& registry,
                              const std::vector<ScriptCommandConfig>& commands) {
    for (const auto& configured : commands) {
        std::optional<CommandButton> button;
        if (configured.button_text) {
            button = CommandButton{
                .text = *configured.button_text,
                .row = configured.button_row,
            };
        }

        registry.add({
            .name = configured.name,
            .description = configured.description,
            .button = std::move(button),
            .handler = [script = configured.script](const CommandContext&) {
                return CommandResponse{.text = run_script(script)};
            },
            .requires_2fa = configured.requires_2fa,
            .enabled = configured.enabled,
        });
    }
}

}  // namespace tbot
