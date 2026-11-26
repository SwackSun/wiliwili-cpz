// SPDX-License-Identifier: GPL-3.0-only
#include "lcd-orientation.hpp"

#include <algorithm>
#include <cmath>
#include <fstream>
#include <optional>
#include <string>

namespace cm0 {
namespace fs = std::filesystem;
namespace {
fs::path findAccelerometer() {
    std::error_code error;
    fs::directory_iterator it("/sys/bus/iio/devices", error), end;
    for (; !error && it != end; it.increment(error)) {
        std::string name;
        if (std::ifstream(it->path() / "name") >> name && name == "bmi270") return it->path();
    }
    return {};
}

std::optional<TopEdge> topEdge(double x, double y, double z) {
    // CardputerZero BMI270 axes in screen coordinates: right = Y, up = -X.
    const double magnitude = std::sqrt(x*x + y*y + z*z);
    const double horizontal = std::abs(y), vertical = std::abs(x);
    const double dominant = std::max(horizontal, vertical);
    if (!(magnitude >= 7.0 && magnitude <= 12.5) || dominant < 0.65 * magnitude ||
        dominant < 1.3 * std::min(horizontal, vertical)) return {};
    if (horizontal > vertical) return y > 0 ? TopEdge::Right : TopEdge::Left;
    return x < 0 ? TopEdge::Top : TopEdge::Bottom;
}
}

bool LcdOrientation::poll(Clock::time_point now) {
    if (now < nextSample_) return false;
    nextSample_ = now + std::chrono::milliseconds(100);
    if (sensor_.empty()) {
        if (now < nextScan_) return false;
        nextScan_ = now + std::chrono::seconds(2);
        sensor_ = findAccelerometer();
        if (sensor_.empty()) return false;
    }
    double x, y, z, scale;
    if (!(std::ifstream(sensor_ / "in_accel_x_raw") >> x) ||
        !(std::ifstream(sensor_ / "in_accel_y_raw") >> y) ||
        !(std::ifstream(sensor_ / "in_accel_z_raw") >> z) ||
        !(std::ifstream(sensor_ / "in_accel_scale") >> scale)) {
        sensor_.clear();
        candidate_ = current_;
        return false;
    }
    const auto edge = topEdge(x * scale, y * scale, z * scale).value_or(current_);
    if (candidate_ != edge) {
        candidate_ = edge;
        candidateSince_ = now;
    }
    if (edge == current_ || now - candidateSince_ < std::chrono::milliseconds(300)) return false;
    current_ = edge;
    return true;
}

void rotateLcdFrame(const std::vector<std::uint32_t>& source, int width, int height,
                    TopEdge edge, std::vector<std::uint32_t>& output) {
    output.resize(source.size());
    if (edge == TopEdge::Bottom) {
        std::reverse_copy(source.begin(), source.end(), output.begin());
    } else if (edge == TopEdge::Top) {
        std::copy(source.begin(), source.end(), output.begin());
    } else {
        for (int x = 0; x < width; ++x) {
            auto* row = output.data() + (edge == TopEdge::Right ? x : width-1-x) * height;
            for (int y = 0; y < height; ++y)
                row[edge == TopEdge::Right ? height-1-y : y] = source[y*width+x];
        }
    }
}
}
