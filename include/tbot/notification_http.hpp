#pragma once

#include "tbot/config.hpp"
#include <functional>
#include <memory>
#include <string>

namespace tbot {

// The callback atomically enqueues a message, or returns false when full.
class NotificationHttpServer {
public:
    NotificationHttpServer(const NotificationHttpConfig& config,
                           std::function<bool(std::string)> enqueue);
    ~NotificationHttpServer();
    NotificationHttpServer(const NotificationHttpServer&) = delete;
    NotificationHttpServer& operator=(const NotificationHttpServer&) = delete;
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace tbot
