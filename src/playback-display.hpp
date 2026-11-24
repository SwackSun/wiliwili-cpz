// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <mpv/client.h>
#include <nanovg.h>

namespace cm0 {
bool usingLcd();
void setDisplayResume(bool playing);
void attachPlayer(mpv_handle* handle, bool lowQuality);
void setVideoFullscreen(bool fullscreen);
bool drawVideoPlane(NVGcontext* vg, float x, float y, float w, float h, float alpha);
}
