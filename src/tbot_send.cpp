#include "tbot/notification_protocol.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <string_view>
#include <utility>

namespace {

struct CliOptions {
    std::filesystem::path socket_path{tbot::default_notification_socket_path};
    std::string message;
};

enum class ParseResult {
    continue_running,
    help_shown,
    error,
};

class UnixNotificationSender {
public:
    enum class Status {
        success,
        invalid_socket_path,
        socket_creation_failed,
        send_failed,
        incomplete_send,
    };

    struct Result {
        Status status;
        int system_error{0};
    };

    explicit UnixNotificationSender(std::filesystem::path socket_path)
        : socket_path_(std::move(socket_path)) {}

    Result send(std::string_view message) const;

private:
    std::filesystem::path socket_path_;
};

void print_help(std::string_view program) {
    std::cout << "Send a local notification to the OpenWrt Telegram bot.\n\n"
              << "Usage:\n"
              << "  " << program << " [--socket <path>] <message>\n"
              << "  printf '%s\\n' 'message' | " << program << " [--socket <path>]\n\n"
              << "The message is passed to the local daemon without waiting for Telegram delivery.\n";
}

void print_message_too_long_error() {
    std::cerr << "Error: message must not exceed "
              << tbot::maximum_notification_bytes << " bytes.\n";
}

bool append_message_part(std::string& message, std::string_view part) {
    const std::size_t separator = message.empty() ? 0 : 1;
    if (message.size() + separator + part.size() >
        tbot::maximum_notification_bytes) {
        return false;
    }
    if (separator != 0) {
        message.push_back(' ');
    }
    message.append(part);
    return true;
}

ParseResult parse_arguments(int argc, char* argv[], CliOptions& options) {
    bool options_enabled = true;

    for (int i = 1; i < argc; ++i) {
        const std::string_view argument(argv[i]);
        if (options_enabled && (argument == "-h" || argument == "--help")) {
            print_help(argv[0]);
            return ParseResult::help_shown;
        }
        if (options_enabled && argument == "--") {
            options_enabled = false;
            continue;
        }
        if (options_enabled && argument == "--socket") {
            if (++i >= argc) {
                std::cerr << "Error: missing path after --socket.\n";
                return ParseResult::error;
            }
            options.socket_path = argv[i];
            continue;
        }
        if (options_enabled && argument.starts_with('-')) {
            std::cerr << "Error: unknown option " << argument << '\n';
            return ParseResult::error;
        }
        if (!append_message_part(options.message, argument)) {
            print_message_too_long_error();
            return ParseResult::error;
        }
    }

    return ParseResult::continue_running;
}

bool read_message_from_stdin(std::string& message) {
    char buffer[512];
    while (std::cin) {
        std::cin.read(buffer, sizeof(buffer));
        const auto count = static_cast<std::size_t>(std::cin.gcount());
        if (message.size() + count > tbot::maximum_notification_bytes) {
            print_message_too_long_error();
            return false;
        }
        message.append(buffer, count);
    }
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r')) {
        message.pop_back();
    }
    return true;
}

bool obtain_message(std::string& message) {
    if (!message.empty()) {
        return true;
    }
    if (isatty(STDIN_FILENO)) {
        std::cerr << "Error: missing message. Provide it as an argument or through standard input.\n";
        return false;
    }
    if (!read_message_from_stdin(message)) {
        return false;
    }
    if (message.empty()) {
        std::cerr << "Error: message must not be empty.\n";
        return false;
    }
    return true;
}

UnixNotificationSender::Result UnixNotificationSender::send(
    std::string_view message) const {
    const std::string socket_text = socket_path_.string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (socket_text.empty() || socket_text.size() >= sizeof(address.sun_path)) {
        return {.status = Status::invalid_socket_path};
    }
    std::memcpy(address.sun_path, socket_text.c_str(), socket_text.size() + 1);

    const int socket_fd = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (socket_fd < 0) {
        return {.status = Status::socket_creation_failed, .system_error = errno};
    }

    const ssize_t sent = sendto(socket_fd,
                                message.data(),
                                message.size(),
                                MSG_DONTWAIT,
                                reinterpret_cast<const sockaddr*>(&address),
                                sizeof(address));
    const int send_error = errno;
    close(socket_fd);

    if (sent < 0) {
        return {.status = Status::send_failed, .system_error = send_error};
    }
    if (static_cast<std::size_t>(sent) != message.size()) {
        return {.status = Status::incomplete_send};
    }
    return {.status = Status::success};
}

int report_send_result(const UnixNotificationSender::Result& result) {
    using Status = UnixNotificationSender::Status;

    switch (result.status) {
        case Status::success:
            return 0;
        case Status::invalid_socket_path:
            std::cerr << "Error: socket path is empty or too long.\n";
            return 2;
        case Status::socket_creation_failed:
            std::cerr << "Error: failed to create socket: "
                      << std::strerror(result.system_error) << '\n';
            return 1;
        case Status::send_failed:
            std::cerr << "Error: failed to pass message to daemon: "
                      << std::strerror(result.system_error) << '\n';
            return 1;
        case Status::incomplete_send:
            std::cerr << "Error: the full message was not passed to the daemon.\n";
            return 1;
    }

    return 1;
}

}  // namespace

int main(int argc, char* argv[]) {
    CliOptions options;
    switch (parse_arguments(argc, argv, options)) {
        case ParseResult::help_shown:
            return 0;
        case ParseResult::error:
            return 2;
        case ParseResult::continue_running:
            break;
    }

    if (!obtain_message(options.message)) {
        return 2;
    }

    const UnixNotificationSender sender(std::move(options.socket_path));
    return report_send_result(sender.send(options.message));
}
