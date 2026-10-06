#include "tbot/qr_code.hpp"

#include <cassert>
#include <stdexcept>
#include <string>

namespace {

void verify_finder(const tbot::QrCode& qr, int left, int top) {
    for (int y = 0; y < 7; ++y) {
        for (int x = 0; x < 7; ++x) {
            const bool expected = x == 0 || x == 6 || y == 0 || y == 6 ||
                                  (x >= 2 && x <= 4 && y >= 2 && y <= 4);
            assert(qr.module(left + x, top + y) == expected);
        }
    }
}

std::size_t count_lines(const std::string& text) {
    std::size_t result = 0;
    for (char character : text) result += character == '\n';
    return result;
}

std::size_t count_occurrences(std::string_view text, std::string_view needle) {
    std::size_t result = 0;
    for (std::size_t position = 0;
         (position = text.find(needle, position)) != std::string_view::npos;
         position += needle.size()) ++result;
    return result;
}

}  // namespace

int main() {
    const std::string uri =
        "otpauth://totp/tbot-wrt:router?secret="
        "GEZDGNBVGY3TQOJQGEZDGNBVGY3TQOJQ"
        "&issuer=tbot-wrt&algorithm=SHA1&digits=6&period=30";
    const tbot::QrCode qr(uri);
    assert(qr.size() >= 21 && qr.size() <= 57);
    assert(!qr.module(-1, 0));
    assert(!qr.module(0, -1));
    assert(!qr.module(qr.size(), 0));
    verify_finder(qr, 0, 0);
    verify_finder(qr, qr.size() - 7, 0);
    verify_finder(qr, 0, qr.size() - 7);

    const auto ansi = qr.render_terminal();
    assert(ansi.starts_with("\x1b[30;107m"));
    assert(ansi.ends_with("\x1b[0m\n"));
    assert(count_lines(ansi) == static_cast<std::size_t>((qr.size() + 8 + 1) / 2));
    assert(count_occurrences(ansi, "\x1b[30;107m") == count_lines(ansi));
    assert(count_occurrences(ansi, "\x1b[0m") == count_lines(ansi));

    const auto ascii = qr.render_terminal(false);
    assert(ascii.find('\x1b') == std::string::npos);
    assert(count_lines(ascii) == static_cast<std::size_t>(qr.size() + 8));

    bool null_rejected = false;
    try {
        const tbot::QrCode invalid(std::string_view("a\0b", 3));
    } catch (const std::invalid_argument&) {
        null_rejected = true;
    }
    assert(null_rejected);

    bool long_text_rejected = false;
    try {
        const tbot::QrCode invalid(std::string(1000, 'x'));
    } catch (const std::runtime_error&) {
        long_text_rejected = true;
    }
    assert(long_text_rejected);
}
