// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bilibili/result/video_detail_result.h"
#include <algorithm>

namespace cm0 {
// Ordinary 720p is QN 64. The live service uses a separate tier numbering scheme.
constexpr int maxVideoQuality = 64;

inline int outputVideoQuality(int preferred, bool lcd) {
    return std::min(preferred, lcd ? 16 : maxVideoQuality);
}

inline void limitVideoQuality(bilibili::VideoUrlResult& result, int requested = maxVideoQuality) {
    requested = std::min(requested, maxVideoQuality);
    const int shortSideLimit = requested <= 16 ? 360 : 720;
    auto& streams = result.dash.video;
    streams.erase(std::remove_if(streams.begin(), streams.end(), [requested, shortSideLimit](const auto& s) {
        const int shortSide = std::min(s.width, s.height);
        return s.id <= 0 || s.id > requested || shortSide <= 0 || shortSide > shortSideLimit || std::max(s.width, s.height) > 1280;
    }), streams.end());

    // Legacy URLs have no dimensions. Only their reported quality identifies the tier.
    if (result.quality <= 0 || result.quality > requested) result.durl.clear();
    result.quality = std::min(result.quality, requested);
    auto& ids = result.accept_quality;
    auto& labels = result.accept_description;
    const auto count = std::min(ids.size(), labels.size());
    size_t kept = 0;
    for (size_t i = 0; i < count; ++i) {
        if (ids[i] <= 0 || ids[i] > maxVideoQuality) continue;
        if (kept != i) {
            ids[kept] = ids[i];
            labels[kept] = std::move(labels[i]);
        }
        ++kept;
    }
    if (streams.empty() && result.durl.empty()) kept = 0;
    ids.resize(kept);
    labels.resize(kept);
}
}
