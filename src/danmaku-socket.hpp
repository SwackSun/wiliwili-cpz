// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <atomic>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

namespace cm0 {

inline bool isChatHost(std::string_view host) {
    constexpr std::string_view suffix = ".chat.bilibili.com";
    if (host.size() <= suffix.size() || host.size() > 253 ||
        host.substr(host.size() - suffix.size()) != suffix) return false;
    size_t start = 0;
    for (size_t i = 0; i <= host.size(); ++i) {
        if (i == host.size() || host[i] == '.') {
            if (i == start || i - start > 63 || host[start] == '-' || host[i - 1] == '-') return false;
            start = i + 1;
        } else if (!(host[i] >= 'a' && host[i] <= 'z') &&
                   !(host[i] >= '0' && host[i] <= '9') && host[i] != '-') {
            return false;
        }
    }
    return true;
}

// Returns true once the join packet has been sent on a verified connection.
bool runDanmakuSocket(const std::string& url, const char* caFile, const std::vector<uint8_t>& join,
                      const std::vector<uint8_t>& heartbeat, const std::atomic<bool>& running,
                      const std::function<void(const std::string&)>& onMessage);

}  // namespace cm0
