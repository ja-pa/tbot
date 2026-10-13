#include "tbot/application.hpp"
#include "tbot/commands.hpp"
#include "tbot/config.hpp"
#include "tbot/qr_code.hpp"

#include <atomic>
#include <charconv>
#include <csignal>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string_view>
#include <unistd.h>

#include <nlohmann/json.hpp>

namespace {

std::atomic_bool stop_requested{false};

void request_stop(int) {
    stop_requested.store(true);
}

void print_help(std::string_view program) {
    std::cout
        << "tbot 0.1.1\n\n"
        << "Usage:\n"
        << "  " << program << " -c <file>\n"
        << "  " << program << " -g <user_id> [-a]\n\n"
        << "Options:\n"
        << "  -g, --generate-2fa-secret <user_id>\n"
        << "                         Generate JSON, an otpauth URI and a QR code\n"
        << "  -a, --qr-ascii          Render QR without ANSI colors or Unicode\n"
        << "  -c, --config <file>     Path to the JSON configuration\n"
        << "  -h, --help             Show this help\n\n"
        << "Without -c, the application only shows this help and does not start.\n";
}

std::optional<std::int64_t> parse_user_id(std::string_view value) {
    std::int64_t result{};
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size() || result <= 0) {
        return std::nullopt;
    }
    return result;
}

}  // namespace

int main(int argc, char* argv[]) {
    std::optional<std::filesystem::path> config_path;
    std::optional<std::int64_t> generate_for_user;
    bool qr_ascii = false;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (argument == "-h" || argument == "--help") {
            print_help(argv[0]);
            return 0;
        }
        if (argument == "-g" || argument == "--generate-2fa-secret") {
            if (++i >= argc) {
                std::cerr << "Error: missing Telegram user_id after " << argument << ".\n\n";
                print_help(argv[0]);
                return 2;
            }
            generate_for_user = parse_user_id(argv[i]);
            if (!generate_for_user) {
                std::cerr << "Error: user_id must be a positive integer.\n";
                return 2;
            }
            continue;
        }
        if (argument == "-a" || argument == "--qr-ascii") {
            qr_ascii = true;
            continue;
        }
        if (argument == "-c" || argument == "--config") {
            if (++i >= argc) {
                std::cerr << "Error: missing file path after " << argument << ".\n\n";
                print_help(argv[0]);
                return 2;
            }
            config_path = argv[i];
            continue;
        }

        std::cerr << "Error: unknown option " << argument << "\n\n";
        print_help(argv[0]);
        return 2;
    }

    if (qr_ascii && !generate_for_user) {
        std::cerr << "The -a/--qr-ascii option can only be used with "
                     "-g/--generate-2fa-secret.\n";
        return 2;
    }
    if (generate_for_user) {
        if (config_path) {
            std::cerr << "Secret generation cannot be combined with --config.\n";
            return 2;
        }
        try {
            const auto secret = tbot::TwoFactor::generate_secret();
            const std::string user_id = std::to_string(*generate_for_user);
            const std::string uri = "otpauth://totp/tbot-wrt:" + user_id + "?secret=" + secret +
                "&issuer=tbot-wrt&algorithm=SHA1&digits=6&period=30";
            const tbot::QrCode qr(uri);
            const nlohmann::json user = {
                {"user_id", *generate_for_user},
                {"secret", secret},
            };
            std::cout << "Entry for telegram.two_factor_users:\n"
                      << user.dump(2) << "\n\notpauth URI:\n" << uri << "\n\n"
                      << qr.render_terminal(!qr_ascii && isatty(STDOUT_FILENO));
            return 0;
        } catch (const std::exception& error) {
            std::cerr << error.what() << '\n';
            return 1;
        }
    }
    if (!config_path) {
        print_help(argv[0]);
        return 0;
    }

    std::signal(SIGINT, request_stop);
    std::signal(SIGTERM, request_stop);

    try {
        auto config = tbot::TbotConfig::load(*config_path);
        tbot::CommandRegistry commands;
        tbot::register_builtin_commands(commands);
        tbot::register_script_commands(commands, config.commands().items());

        tbot::Application application(std::move(config), std::move(commands));
        application.run(stop_requested);
    } catch (const std::exception& error) {
        std::cerr << "Error: " << error.what() << '\n';
        return 1;
    }

    return 0;
}
