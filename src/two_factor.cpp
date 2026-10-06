#include "tbot/two_factor.hpp"
extern "C" {
#include "tiny2fa.h"
}
#include <array>
#include <cerrno>
#include <stdexcept>
#include <sys/random.h>
#include <unordered_set>

namespace tbot {
TwoFactor::TwoFactor(Secrets secrets, SteadyNow steady, UnixNow unix_now)
    : secrets_(std::move(secrets)), steady_(std::move(steady)), unix_now_(std::move(unix_now)) {
    if (!unix_now_) unix_now_ = [] {
        return std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch()).count();
    };
    std::unordered_set<std::string> unique;
    for (const auto& [id, secret] : secrets_) {
        if (id <= 0 || !valid_secret(secret) || !unique.insert(secret).second)
            throw std::runtime_error("2FA requires unique secrets: 32 characters from A-Z and 2-7, and positive user_id values");
    }
}
bool TwoFactor::valid_secret(std::string_view secret) {
    if (secret.size() != 32) return false;
    for (char c : secret) if (!((c >= 'A' && c <= 'Z') || (c >= '2' && c <= '7'))) return false;
    return true;
}
std::string TwoFactor::generate_secret() {
    std::array<unsigned char, 32> random{};
    std::size_t offset = 0;
    while (offset < random.size()) {
        const auto n = getrandom(random.data() + offset, random.size() - offset, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) throw std::runtime_error("Cannot obtain secure randomness for 2FA");
        offset += static_cast<std::size_t>(n);
    }
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    std::string result;
    for (auto byte : random) result += alphabet[byte & 31];
    return result;
}
bool TwoFactor::verify(std::int64_t user, std::string_view code) {
    const auto secret = secrets_.find(user);
    if (secret == secrets_.end()) return false;
    auto& state = states_[user];
    const auto now = steady_();
    if (now < state.blocked_until) return false;
    int value = 0;
    bool valid = code.size() == 6;
    for (char c : code) {
        if (c < '0' || c > '9') { valid = false; break; }
        if (valid) value = value * 10 + c - '0';
    }
    const auto step = unix_now_() / 30;
    std::int64_t matched = -1;
    if (valid && step > 0) {
        for (auto candidate = step - 1; candidate <= step + 1; ++candidate) {
            if (candidate > state.last_step &&
                t2_get_key(reinterpret_cast<const uint8_t*>(secret->second.c_str()),
                           static_cast<uint64_t>(candidate * 30 + 1)) == value)
                matched = candidate;
        }
    }
    if (matched >= 0) {
        state.last_step = matched;
        state.expires = now + std::chrono::minutes(5);
        state.failures = 0;
        return true;
    }
    if (++state.failures >= 5) {
        state.blocked_until = now + std::chrono::minutes(5);
        state.failures = 0;
    }
    return false;
}
bool TwoFactor::authenticated(std::int64_t user) const {
    const auto state = states_.find(user);
    return state != states_.end() && steady_() < state->second.expires;
}
void TwoFactor::lock(std::int64_t user) {
    const auto state = states_.find(user);
    if (state != states_.end()) state->second.expires = {};
}
}
