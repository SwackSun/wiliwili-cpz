// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <vector>

namespace cm0 {

// The edge of the physical panel that should display the top of the UI.
enum class TopEdge { Top, Right, Bottom, Left };

inline bool portrait(TopEdge edge) {
    return edge == TopEdge::Left || edge == TopEdge::Right;
}

class LcdOrientation {
public:
    using Clock = std::chrono::steady_clock;
    bool poll(Clock::time_point now);
    TopEdge edge() const { return current_; }

private:
    std::filesystem::path sensor_;
    Clock::time_point nextSample_{}, nextScan_{}, candidateSince_{};
    TopEdge current_ = TopEdge::Top, candidate_ = TopEdge::Top;
};

void rotateLcdFrame(const std::vector<std::uint32_t>& source, int width, int height,
                    TopEdge edge, std::vector<std::uint32_t>& output);

}
