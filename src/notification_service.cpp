#include "tbot/notification_service.hpp"

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>
#include <poll.h>

#include <cerrno>
#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <utility>

namespace tbot {
namespace {

constexpr auto send_interval = std::chrono::seconds(1);

sockaddr_un make_socket_address(const std::filesystem::path& path) {
    const std::string text = path.string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (text.size() >= sizeof(address.sun_path)) {
        throw std::runtime_error("Cesta k notification socketu je příliš dlouhá: " + text);
    }
    std::memcpy(address.sun_path, text.c_str(), text.size() + 1);
    return address;
}

}  // namespace

bool NotificationQueue::push(std::string message) {
    const bool discarded = messages_.size() == maximum_queued_notifications;
    if (discarded) {
        messages_.pop_front();
    }
    messages_.push_back(std::move(message));
    return discarded;
}

bool NotificationQueue::try_pop(std::string& message) {
    if (messages_.empty()) {
        return false;
    }
    message = std::move(messages_.front());
    messages_.pop_front();
    return true;
}

std::size_t NotificationQueue::size() const noexcept {
    return messages_.size();
}

NotificationService::NotificationService(std::string telegram_token,
                                         std::vector<std::int64_t> chat_ids,
                                         std::filesystem::path socket_path)
    : telegram_(std::move(telegram_token)),
      chat_ids_(std::move(chat_ids)),
      socket_path_(std::move(socket_path)) {
    socket_fd_ = socket(AF_UNIX, SOCK_DGRAM | SOCK_CLOEXEC, 0);
    if (socket_fd_ < 0) {
        throw std::runtime_error("Vytvoření notification socketu selhalo: " +
                                 std::string(std::strerror(errno)));
    }

    try {
        struct stat existing{};
        if (lstat(socket_path_.c_str(), &existing) == 0) {
            if (!S_ISSOCK(existing.st_mode)) {
                throw std::runtime_error("Cesta notification socketu už existuje a není socket: " +
                                         socket_path_.string());
            }
            if (unlink(socket_path_.c_str()) != 0) {
                throw std::runtime_error("Odstranění starého notification socketu selhalo: " +
                                         std::string(std::strerror(errno)));
            }
        } else if (errno != ENOENT) {
            throw std::runtime_error("Kontrola notification socketu selhala: " +
                                     std::string(std::strerror(errno)));
        }

        const auto address = make_socket_address(socket_path_);
        if (bind(socket_fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
            throw std::runtime_error("Připojení notification socketu selhalo: " +
                                     std::string(std::strerror(errno)));
        }
        if (chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR) != 0) {
            throw std::runtime_error("Nastavení oprávnění notification socketu selhalo: " +
                                     std::string(std::strerror(errno)));
        }

        receiver_ = std::thread(&NotificationService::receive_loop, this);
        sender_ = std::thread(&NotificationService::send_loop, this);
    } catch (...) {
        stopping_.store(true);
        queue_changed_.notify_all();
        if (receiver_.joinable()) {
            receiver_.join();
        }
        if (sender_.joinable()) {
            sender_.join();
        }
        close(socket_fd_);
        socket_fd_ = -1;
        unlink(socket_path_.c_str());
        throw;
    }

    std::cout << "Lokální notifikace poslouchají na " << socket_path_ << '\n';
}

NotificationService::~NotificationService() {
    stopping_.store(true);
    queue_changed_.notify_all();
    if (receiver_.joinable()) {
        receiver_.join();
    }
    if (sender_.joinable()) {
        sender_.join();
    }
    if (socket_fd_ >= 0) {
        close(socket_fd_);
    }
    unlink(socket_path_.c_str());
}

void NotificationService::receive_loop() {
    std::vector<char> buffer(maximum_notification_bytes + 1);
    while (!stopping_.load()) {
        pollfd descriptor{.fd = socket_fd_, .events = POLLIN, .revents = 0};
        const int ready = poll(&descriptor, 1, 250);
        if (ready < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "Čekání na lokální notifikaci selhalo: " << std::strerror(errno) << '\n';
            break;
        }
        if (ready == 0) {
            continue;
        }

        const ssize_t received = recv(socket_fd_, buffer.data(), buffer.size(), MSG_DONTWAIT);
        if (received < 0) {
            if (errno == EINTR) {
                continue;
            }
            if (!stopping_.load()) {
                std::cerr << "Příjem lokální notifikace selhal: " << std::strerror(errno) << '\n';
            }
            break;
        }
        if (received == 0 || static_cast<std::size_t>(received) > maximum_notification_bytes) {
            std::cerr << "Lokální notifikace byla prázdná nebo příliš dlouhá; zahazuji ji.\n";
            continue;
        }

        bool discarded = false;
        {
            std::lock_guard lock(mutex_);
            discarded = queue_.push(std::string(buffer.data(), static_cast<std::size_t>(received)));
        }
        if (discarded) {
            std::cerr << "Fronta lokálních notifikací je plná; nejstarší zpráva byla zahozena.\n";
        }
        queue_changed_.notify_one();
    }
}

void NotificationService::send_loop() {
    auto next_send = std::chrono::steady_clock::now();
    while (!stopping_.load()) {
        std::string message;
        {
            std::unique_lock lock(mutex_);
            queue_changed_.wait(lock, [&] { return stopping_.load() || queue_.size() != 0; });
            if (stopping_.load()) {
                return;
            }
            const auto now = std::chrono::steady_clock::now();
            if (now < next_send) {
                queue_changed_.wait_until(lock, next_send, [&] { return stopping_.load(); });
                if (stopping_.load()) {
                    return;
                }
            }
            queue_.try_pop(message);
        }

        for (const auto chat_id : chat_ids_) {
            try {
                telegram_.send_message(chat_id, message);
            } catch (const std::exception& error) {
                std::cerr << "Odeslání lokální notifikace do chat_id=" << chat_id
                          << " selhalo: " << error.what() << '\n';
            }
        }
        next_send = std::chrono::steady_clock::now() + send_interval;
    }
}

}  // namespace tbot
