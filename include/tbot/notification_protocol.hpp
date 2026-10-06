#pragma once

#include <cstddef>
#include <string_view>

namespace tbot {

inline constexpr std::size_t maximum_notification_bytes = 3500;
inline constexpr std::size_t maximum_queued_notifications = 10;
inline constexpr std::string_view default_notification_socket_path = "/tmp/tbot-wrt.sock";

}  // namespace tbot
