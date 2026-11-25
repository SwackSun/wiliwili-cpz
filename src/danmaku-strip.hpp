// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string_view>

namespace cm0 {
class DanmakuStrip {
public:
    static constexpr int width = 1280;
    static constexpr int height = 192;
    using Rect = std::array<int, 4>;

    DanmakuStrip();
    ~DanmakuStrip();
    bool ready() const;
    bool active() const;
    bool paint(std::string_view text, int size, uint32_t color, unsigned alpha,
               int x, int y, const Rect& clip);
    void copyTo(unsigned page, uint8_t* target, unsigned pitch);
    void clear();

private:
    struct State;
    std::unique_ptr<State> state;
};
}
