// SPDX-License-Identifier: GPL-3.0-only
#include "bilibili/util/http.hpp"
#include "account-policy.hpp"
#include "account-http.hpp"
#include <mutex>

namespace bilibili {
namespace {
cpr::Cookies guest{false};
}

void HTTP::initGuest(const Cookies& cookies) {
    guest = cpr::Cookies{false};
    guest.emplace_back({"DedeUserID", "0"});
    auto id = cookies.find("buvid3");
    if (id != cookies.end()) guest.emplace_back({id->first, id->second});
}

std::shared_ptr<cpr::Session> HTTP::createApiSession(const std::string& link, const cpr::Cookies* pollDevice) {
    auto session = createSession();
    const auto url = parseLink(link);
    session->SetUrl(cpr::Url{url});
    // API payloads (including CSRF and QR poll keys) must stay at the original endpoint.
    session->SetRedirect(cpr::Redirect{false});
    auto headers = HEADERS;
    if (cm0::accountApi(url)) {
        cm0::checkAccount();
        const auto cookies = cm0::validCookies(cm0::currentAccount());
        if (cookies.empty()) {
            if (url != cm0::logoutUrl) headers["Cookie"] = getEncodedCookie(guest);
        } else {
            cpr::Cookies values{false};
            for (const auto& item : cookies) values.emplace_back({item.first, item.second});
            headers["Cookie"] = getEncodedCookie(values);
        }
    } else if (pollDevice && url == cm0::pollUrl) {
        headers["Cookie"] = getEncodedCookie(*pollDevice);
    }
    session->SetHeader(headers);
    return session;
}
}

namespace cm0 {
namespace {
std::mutex loginMutex;
std::string selectedKey;
}

void selectLoginKey(const std::string& key) {
    std::lock_guard<std::mutex> guard(loginMutex);
    selectedKey = key;
}

void cancelLoginKey(const std::string& key) {
    std::lock_guard<std::mutex> guard(loginMutex);
    if (key == selectedKey) selectedKey.clear();
}

bool acceptLogin(uint64_t generation, const cpr::Cookies& response, const std::string& refresh, CookieMap cookies,
                 const std::string& key) {
    std::lock_guard<std::mutex> guard(loginMutex);
    if (!key.empty() && key != selectedKey) return false;
    Account a;
    a.cookies = std::move(cookies);
    a.refresh = refresh;
    for (const auto& c : response) {
        a.cookies[c.GetName()] = c.GetValue();
        a.expires[c.GetName()] = std::chrono::system_clock::to_time_t(c.GetExpires());
    }
    auto valid = validCookies(a);
    return !valid["SESSDATA"].empty() && !valid["bili_jct"].empty() && saveAccount(generation, std::move(a));
}
}
