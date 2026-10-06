#pragma once

extern "C" {
#include "qrcodegen.h"
}

#include <array>
#include <cstdint>
#include <string>
#include <string_view>

namespace tbot {

class QrCode {
public:
    explicit QrCode(std::string_view text);

    int size() const noexcept;
    bool module(int x, int y) const noexcept;
    std::string render_terminal(bool ansi_colors = true) const;

private:
    static constexpr int maximum_version = 10;
    static constexpr std::size_t buffer_size =
        qrcodegen_BUFFER_LEN_FOR_VERSION(maximum_version);

    std::array<std::uint8_t, buffer_size> data_{};
    int size_{};
};

}  // namespace tbot
