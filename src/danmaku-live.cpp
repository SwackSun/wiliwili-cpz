// SPDX-License-Identifier: GPL-3.0-only
// Adapts wiliwili v1.6.0's danmaku_live.cpp, originally created by maye174 on 2023-04-06.
#include "danmaku_live.hpp"
#include "bilibili.h"
#include "bilibili/api.h"
#include "bilibili/util/http.hpp"
#include "danmaku-socket.hpp"
#include "live/ws_utils.hpp"
#include "utils/config_helper.hpp"

void bilibili::BilibiliClient::get_live_danmaku_info(int roomid, const std::function<void(LiveDanmakuinfo)>& callback,
                                                    const ErrorCallback& error) {
    HTTP::getResultWithWbiAsync<LiveDanmakuinfo>(Api::LiveDanmakuInfo,
                                               {{"type", "0"}, {"id", std::to_string(roomid)}},
                                               callback, error);
}

LiveDanmaku::~LiveDanmaku() {
    disconnect();
}

void LiveDanmaku::setonMessage(MessageCallback callback) {
    std::lock_guard<std::mutex> lock(mutex);
    onMessage = std::move(callback);
}

void LiveDanmaku::connect(int roomId, uint64_t uid, const bilibili::LiveDanmakuinfo& info) {
    std::lock_guard<std::mutex> lock(mutex);
    if (running)
        return;
    if (worker.joinable())
        worker.join();

    std::vector<std::string> urls;
    for (auto it = info.host_list.rbegin(); it != info.host_list.rend(); ++it) {
        if (it->wss_port > 0 && it->wss_port <= 65535 && cm0::isChatHost(it->host))
            urls.push_back("wss://" + it->host + ":" + std::to_string(it->wss_port) + "/sub");
    }
    if (urls.empty()) {
        brls::Logger::warning("(LiveDanmaku) No trusted WSS endpoint");
        return;
    }

    nlohmann::json join = {{"uid", uid}, {"roomid", roomId}, {"protover", 2},
                           {"buvid", ProgramConfig::instance().getUuID()}, {"platform", "web"},
                           {"type", 2}, {"key", info.token}};
    auto joinPacket = encode_packet(0, 7, join.dump());
    auto heartbeat = encode_packet(0, 2, "");
    auto callback = onMessage;
    running = true;
    try {
        worker = std::thread([this, urls = std::move(urls), join = std::move(joinPacket),
                              heartbeat = std::move(heartbeat), callback = std::move(callback)] {
            bool joined = false;
            for (const auto& url : urls) {
                if (!running)
                    break;
                joined = cm0::runDanmakuSocket(url, "/etc/ssl/certs/ca-certificates.crt",
                                               join, heartbeat, running, callback);
                if (joined)
                    break;
            }
            if (!joined && running)
                brls::Logger::warning("(LiveDanmaku) Unable to join over WSS");
            running = false;
        });
    } catch (...) {
        running = false;
        throw;
    }
}

void LiveDanmaku::disconnect() {
    std::lock_guard<std::mutex> lock(mutex);
    running = false;
    if (worker.joinable())
        worker.join();
}
