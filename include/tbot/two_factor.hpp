#pragma once
#include <chrono>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace tbot {
class TwoFactor {
public:
    using Secrets = std::unordered_map<std::int64_t, std::string>;
    using Clock = std::chrono::steady_clock;
    using SteadyNow = std::function<Clock::time_point()>;
    using UnixNow = std::function<std::int64_t()>;
    explicit TwoFactor(Secrets secrets, SteadyNow steady = Clock::now,
                       UnixNow unix_now = {});
    static bool valid_secret(std::string_view secret);
    static std::string generate_secret();
    bool verify(std::int64_t user, std::string_view code);
    bool authenticated(std::int64_t user) const;
    void lock(std::int64_t user);
private:
    struct State {
        Clock::time_point expires{};
        Clock::time_point blocked_until{};
        std::int64_t last_step{-1};
        unsigned failures{};
    };
    Secrets secrets_;
    std::unordered_map<std::int64_t, State> states_;
    SteadyNow steady_;
    UnixNow unix_now_;
};
}
