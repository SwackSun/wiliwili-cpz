// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <array>
#include <cstdint>
#include <string_view>
#include <mpv/client.h>
#include <nanovg.h>

namespace brls { class View; }

namespace cm0 {
class DanmakuStrip;
struct RollingDanmaku {
    DanmakuStrip* strip = nullptr;
    float scale = 1, offsetX = 0, offsetY = 0;
    float fontScale = 1;
    std::array<int, 4> clip{};
    unsigned maxLines = 0;
    bool paint(std::string_view text, float size, uint32_t color, float alpha, float x, float y) const;
};

RollingDanmaku rollingDanmaku(NVGcontext* vg, float x, float y, float w, float h,
                            float lineHeight, bool smartMask);
bool usingLcd();
bool compactLayout();
void setClientDisplayLayout(bool lcd);
void refreshVideoLayout(brls::View& video, bool lcd);
void setDisplayResume(bool playing);
void attachPlayer(mpv_handle* handle, bool lowQuality);
void setVideoFullscreen(bool fullscreen);
bool drawVideoPlane(NVGcontext* vg, float x, float y, float w, float h, float alpha);
}
