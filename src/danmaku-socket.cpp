// SPDX-License-Identifier: GPL-3.0-only
#include "danmaku-socket.hpp"

#include <curl/curl.h>
#include <poll.h>
#include <array>
#include <cerrno>
#include <chrono>
#include <memory>

namespace cm0 {
namespace {
using Clock = std::chrono::steady_clock;

bool waitForSocket(curl_socket_t socket, short events) {
    pollfd descriptor{socket, events, 0};
    int result = poll(&descriptor, 1, 100);
    return result >= 0 || errno == EINTR;
}

bool sendPacket(CURL* curl, curl_socket_t socket, const std::vector<uint8_t>& packet,
                const std::atomic<bool>& running) {
    auto deadline = Clock::now() + std::chrono::seconds(5);
    size_t offset = 0;
    while (offset < packet.size() && running && Clock::now() < deadline) {
        size_t sent = 0;
        auto result = curl_ws_send(curl, packet.data() + offset, packet.size() - offset, &sent, 0, CURLWS_BINARY);
        offset += sent;
        if (result == CURLE_OK && offset == packet.size())
            return true;
        if (result != CURLE_OK && result != CURLE_AGAIN)
            return false;
        if (!waitForSocket(socket, POLLOUT))
            return false;
    }
    return false;
}

int stopTransfer(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
    return !*static_cast<const std::atomic<bool>*>(context);
}
}  // namespace

bool runDanmakuSocket(const std::string& url, const char* caFile, const std::vector<uint8_t>& join,
                      const std::vector<uint8_t>& heartbeat, const std::atomic<bool>& running,
                      const std::function<void(const std::string&)>& onMessage) {
    std::unique_ptr<CURL, decltype(&curl_easy_cleanup)> handle(curl_easy_init(), curl_easy_cleanup);
    CURL* curl = handle.get();
    if (!curl || !running)
        return false;

    if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_PROTOCOLS_STR, "wss") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECT_ONLY, 2L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CAINFO, caFile) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_PROXY, "") != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, stopTransfer) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &running) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, 5000L) != CURLE_OK ||
        curl_easy_perform(curl) != CURLE_OK)
        return false;

    curl_socket_t socket = CURL_SOCKET_BAD;
    if (curl_easy_getinfo(curl, CURLINFO_ACTIVESOCKET, &socket) != CURLE_OK ||
        socket == CURL_SOCKET_BAD || !sendPacket(curl, socket, join, running))
        return false;

    constexpr size_t maxMessageSize = 4 * 1024 * 1024;
    constexpr auto heartbeatInterval = std::chrono::seconds(30);
    auto nextHeartbeat = Clock::now() + heartbeatInterval;
    std::array<char, 8192> buffer;
    std::string message;
    while (running) {
        if (Clock::now() >= nextHeartbeat) {
            if (!sendPacket(curl, socket, heartbeat, running))
                break;
            nextHeartbeat = Clock::now() + heartbeatInterval;
        }

        size_t received = 0;
        const curl_ws_frame* frame = nullptr;
        auto result = curl_ws_recv(curl, buffer.data(), buffer.size(), &received, &frame);
        if (result == CURLE_AGAIN) {
            if (!waitForSocket(socket, POLLIN))
                break;
            continue;
        }
        if (result != CURLE_OK || !frame || (frame->flags & CURLWS_CLOSE))
            break;
        if (!(frame->flags & CURLWS_BINARY))
            continue;
        if (received > maxMessageSize - message.size())
            break;
        message.append(buffer.data(), received);
        if (!frame->bytesleft && !(frame->flags & CURLWS_CONT)) {
            if (onMessage && running)
                onMessage(message);
            message.clear();
        }
    }
    return true;
}

}  // namespace cm0
