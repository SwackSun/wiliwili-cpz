// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "bilibili/result/video_detail_result.h"
namespace cm0 {
void prepareVideoStreams(bilibili::VideoUrlResult& result, int landscapeMax, int portraitMax, int codec, int requested);
}
