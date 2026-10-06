#pragma once

#include "tbot/notification_protocol.hpp"
#include "tbot/telegram_client.hpp"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace tbot {

class NotificationQueue {
public:
    // Returns true when the oldest queued message had to be discarded.
    bool push(std::string message);
    bool try_pop(std::string& message);
    std::size_t size() const noexcept;

private:
    std::deque<std::string> messages_;
};

class NotificationService {
public:
    NotificationService(std::string telegram_token,
                        std::vector<std::int64_t> chat_ids,
                        std::filesystem::path socket_path);
    ~NotificationService();

    NotificationService(const NotificationService&) = delete;
    NotificationService& operator=(const NotificationService&) = delete;

private:
    void receive_loop();
    void send_loop();

    TelegramClient telegram_;
    std::vector<std::int64_t> chat_ids_;
    std::filesystem::path socket_path_;
    int socket_fd_{-1};
    std::atomic_bool stopping_{false};
    std::mutex mutex_;
    std::condition_variable queue_changed_;
    NotificationQueue queue_;
    std::thread receiver_;
    std::thread sender_;
};

}  // namespace tbot
