// SPDX-License-Identifier: GPL-3.0-only
#include "video-cdn.hpp"
#include "video-quality.hpp"
#include "bilibili/util/http.hpp"
#include <curl/curl.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <iostream>
#include <string_view>

namespace cm0 {
namespace {
constexpr size_t probeBytes = 128 * 1024;
struct Probe {
    CURL* easy = nullptr;
    size_t received = 0, index = 0;
    std::string url;
};
size_t countBytes(char*, size_t size, size_t count, void* userdata) {
    auto& p = *static_cast<Probe*>(userdata);
    const size_t bytes = size * count;
    if (bytes > probeBytes - p.received)
        return 0;
    p.received += bytes;
    return bytes;
}
std::string mediaHttps(std::string url) {
    if (url.compare(0, 7, "http://") == 0)
        url.replace(0, 4, "https");
    if (url.compare(0, 8, "https://") != 0 || url.find_first_of("@\\\r\n\t ") != url.npos)
        return {};
    const auto end = url.find('/', 8);
    if (end == url.npos)
        return {};
    const std::string_view host(url.data() + 8, end - 8);
    for (std::string_view suffix : {".bilivideo.com", ".bilivideo.cn"}) {
        if (host.size() > suffix.size() && host.substr(host.size() - suffix.size()) == suffix)
            return url;
    }
    return {};
}
void selectEndpoint(bilibili::DashMedia& media) {
    if (media.backup_url.empty())
        return;
    auto* multi = curl_multi_init();
    if (!multi)
        return;
    std::array<Probe, 3> probes{};
    size_t used = 0;
    auto& proxies = bilibili::HTTP::PROXIES;
    for (size_t i = 0; i <= media.backup_url.size() && used < probes.size(); ++i) {
        auto url = mediaHttps(i ? media.backup_url[i - 1] : media.base_url);
        if (url.empty() ||
            std::any_of(probes.begin(), probes.begin() + used, [&](const Probe& p) { return p.url == url; }))
            continue;
        auto& p = probes[used++];
        p.url = std::move(url);
        p.index = i;
        p.easy = curl_easy_init();
        if (!p.easy)
            continue;
        curl_easy_setopt(p.easy, CURLOPT_URL, p.url.c_str());
        curl_easy_setopt(p.easy, CURLOPT_RANGE, "0-131071");
        curl_easy_setopt(p.easy, CURLOPT_REFERER, "https://www.bilibili.com/");
        curl_easy_setopt(p.easy, CURLOPT_USERAGENT, "Mozilla/5.0");
        curl_easy_setopt(p.easy, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(p.easy, CURLOPT_TIMEOUT_MS, 1500L);
        curl_easy_setopt(p.easy, CURLOPT_PRIVATE, &p);
        curl_easy_setopt(p.easy, CURLOPT_WRITEFUNCTION, countBytes);
        curl_easy_setopt(p.easy, CURLOPT_WRITEDATA, &p);
        if (proxies.has("https"))
            curl_easy_setopt(p.easy, CURLOPT_PROXY, proxies["https"].c_str());
        curl_multi_add_handle(multi, p.easy);
    }
    int running = 0;
    Probe* winner = nullptr;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(1500);
    if (used > 1)
        do {
            if (curl_multi_perform(multi, &running) != CURLM_OK)
                break;
            int queued = 0;
            while (auto* done = curl_multi_info_read(multi, &queued)) {
                if (done->msg != CURLMSG_DONE || done->data.result != CURLE_OK)
                    continue;
                Probe* p = nullptr;
                long code = 0;
                curl_easy_getinfo(done->easy_handle, CURLINFO_PRIVATE, &p);
                curl_easy_getinfo(done->easy_handle, CURLINFO_RESPONSE_CODE, &code);
                if (code == 206 && p && p->received == probeBytes) {
                    winner = p;
                    break;
                }
            }
            if (winner || !running || std::chrono::steady_clock::now() >= deadline)
                break;
        } while (curl_multi_poll(multi, nullptr, 0, 50, nullptr) == CURLM_OK);
    if (winner) {
        if (winner->index) std::swap(media.base_url, media.backup_url[winner->index - 1]);
        std::clog << "video CDN probe selected server entry " << winner->index + 1 << '\n';
        media.base_url = winner->url;
    }
    for (auto& p : probes)
        if (p.easy) {
            curl_multi_remove_handle(multi, p.easy);
            curl_easy_cleanup(p.easy);
        }
    curl_multi_cleanup(multi);
}
} // namespace
void prepareVideoStreams(bilibili::VideoUrlResult& result, int landscapeMax, int portraitMax, int codec, int requested) {
    limitVideoQuality(result, requested);
    bilibili::DashMedia* best = nullptr;
    for (auto& stream : result.dash.video) {
        int cap = stream.width >= stream.height ? landscapeMax : portraitMax;
        if (stream.id < 64 || stream.id > std::min(result.quality, cap))
            continue;
        if (!best || stream.id > best->id ||
            (stream.id == best->id && stream.codecid == codec && best->codecid != codec))
            best = &stream;
    }
    if (best)
        selectEndpoint(*best);
}
} // namespace cm0
