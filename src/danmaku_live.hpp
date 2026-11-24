// SPDX-License-Identifier: GPL-3.0-only
// Adapts wiliwili v1.6.0's danmaku_live.hpp, originally created by maye174 on 2023-04-06.
#pragma once

#include "bilibili/result/live_danmaku_result.h"

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <thread>

class LiveDanmaku {
public:
    using MessageCallback = std::function<void(const std::string&)>;

    ~LiveDanmaku();
    void connect(int roomId, uint64_t uid, const bilibili::LiveDanmakuinfo& info);
    void disconnect();
    void setonMessage(MessageCallback callback);

private:
    std::mutex mutex;
    std::thread worker;
    std::atomic<bool> running{false};
    MessageCallback onMessage;
};
