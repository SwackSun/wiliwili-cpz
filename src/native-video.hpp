// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <vector>
#include <mpv/cm0_frame.h>

namespace cm0 {
class NativeVideo {
  public:
    static std::unique_ptr<NativeVideo> open(int uiWidth, int uiHeight);
    ~NativeVideo();
    bool upload(const mpv_cm0_frame& frame, int targetWidth, int targetHeight);
    bool show(const std::vector<uint32_t>& ui, int width, int height);
    void hide();
    void composeScreenshot(std::vector<uint32_t>& ui, int width, int height) const;
    // Visible video rectangle in normalized SW target coordinates, including mpv zoom/pan.
    std::array<float, 4> viewport{};

  private:
    struct State;
    std::unique_ptr<State> state;
    NativeVideo();
};
} // namespace cm0
