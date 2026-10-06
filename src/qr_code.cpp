#include "tbot/qr_code.hpp"

#include <stdexcept>

namespace tbot {
namespace {

constexpr int quiet_zone = 4;

}  // namespace

QrCode::QrCode(std::string_view text) {
    if (text.find('\0') != std::string_view::npos) {
        throw std::invalid_argument("QR code text contains a null character");
    }

    std::array<std::uint8_t, buffer_size> temporary{};
    const std::string terminated(text);
    if (!qrcodegen_encodeText(terminated.c_str(), temporary.data(), data_.data(),
                              qrcodegen_Ecc_MEDIUM, qrcodegen_VERSION_MIN,
                              maximum_version, qrcodegen_Mask_AUTO, true)) {
        throw std::runtime_error("Text is too long for a terminal QR code");
    }
    size_ = qrcodegen_getSize(data_.data());
}

int QrCode::size() const noexcept {
    return size_;
}

bool QrCode::module(int x, int y) const noexcept {
    return qrcodegen_getModule(data_.data(), x, y);
}

std::string QrCode::render_terminal(bool ansi_colors) const {
    std::string result;

    for (int y = -quiet_zone; y < size_ + quiet_zone; y += 2) {
        if (ansi_colors) {
            // Black foreground and white background make scanning reliable on
            // both light and dark terminal themes.
            result += "\x1b[30;107m";
        }
        for (int x = -quiet_zone; x < size_ + quiet_zone; ++x) {
            const bool upper = module(x, y);
            const bool lower = module(x, y + 1);
            if (ansi_colors) {
                if (upper && lower) result += "█";
                else if (upper) result += "▀";
                else if (lower) result += "▄";
                else result += ' ';
            } else {
                result += upper ? "##" : "  ";
                if (y + 1 < size_ + quiet_zone) {
                    // Plain ASCII cannot represent two rows in one character.
                    // The lower row is emitted by the second pass below.
                }
            }
        }
        if (ansi_colors) result += "\x1b[0m\n";
        else {
            result += '\n';
            if (y + 1 < size_ + quiet_zone) {
                for (int x = -quiet_zone; x < size_ + quiet_zone; ++x)
                    result += module(x, y + 1) ? "##" : "  ";
                result += '\n';
            }
        }
    }
    return result;
}

}  // namespace tbot
