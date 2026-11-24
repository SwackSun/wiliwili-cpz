// SPDX-License-Identifier: GPL-3.0-only
#include "account-http.hpp"
#include "account-sync.hpp"
#include "account-policy.hpp"
#include "account-logout.hpp"
#include "bilibili/util/http.hpp"
#include <borealis/core/application.hpp>
#include <borealis/core/thread.hpp>
#include <borealis/views/dialog.hpp>
#include <chrono>

namespace cm0 {
namespace {
std::mutex operation;
AccountReply request(AccountEndpoint endpoint, const Account& a, const CookieMap& form, const std::string& path) {
    using bilibili::HTTP;
    std::string url;
    switch (endpoint) {
        case AccountEndpoint::status: url = "https://passport.bilibili.com/x/passport-login/web/cookie/info"; break;
        case AccountEndpoint::correspond: url = "https://www.bilibili.com/correspond/1/" + path; break;
        case AccountEndpoint::refresh: url = "https://passport.bilibili.com/x/passport-login/web/cookie/refresh"; break;
        case AccountEndpoint::confirm: url = "https://passport.bilibili.com/x/passport-login/web/confirm/refresh"; break;
        case AccountEndpoint::logout: url = logoutUrl; break;
    }
    auto session = HTTP::createSession();
    session->SetUrl(url);
    session->SetRedirect(cpr::Redirect{false});
    cpr::Cookies cookies{false};
    for (const auto& item : validCookies(a)) cookies.emplace_back({item.first, item.second});
    auto headers = HTTP::HEADERS;
    headers["Cookie"] = HTTP::getEncodedCookie(cookies);
    session->SetHeader(headers);
    if (!form.empty()) {
        std::vector<cpr::Pair> pairs;
        for (const auto& item : form) pairs.emplace_back(item.first, item.second);
        session->SetPayload(cpr::Payload(pairs.begin(), pairs.end()));
    }
    auto r = form.empty() ? session->Get() : session->Post();
    AccountReply out;
    if (r.error) return out;
    out.status = r.status_code;
    out.body = std::move(r.text);
    for (const auto& c : r.cookies) {
        out.cookies[c.GetName()] = c.GetValue();
        out.expires[c.GetName()] = std::chrono::system_clock::to_time_t(c.GetExpires());
    }
    return out;
}
void quit() {
    brls::Application::getPlatform()->exitToHomeMode(false);
    brls::Application::quit();
}
}

void checkAccount() {
    using Clock = std::chrono::steady_clock;
    static std::mutex schedule;
    static Clock::time_point next{};
    static uint64_t last = 0;
    auto a = currentAccount();
    if (validCookies(a).empty()) return;
    std::lock_guard<std::mutex> guard(schedule);
    if (a.generation == last && Clock::now() < next) return;
    last = a.generation;
    next = Clock::now() + std::chrono::minutes(5);
    cpr::async([a] {
        std::lock_guard<std::mutex> lock(operation);
        if (currentAccount().generation != a.generation) return;
        try {
            if (synchronize(a, request)) {
                std::lock_guard<std::mutex> guard(schedule);
                if (last == a.generation) next = Clock::now() + std::chrono::hours(1);
            }
        } catch (const std::exception&) {
            brls::Logger::warning("Account status check unavailable");
        }
    });
}

void logout() {
    cpr::async([] {
        std::lock_guard<std::mutex> lock(operation);
        const auto a = currentAccount();
        bool revoked = false;
        try {
            auto csrf = a.cookies.find("bili_jct");
            if (csrf != a.cookies.end()) {
                auto r = request(AccountEndpoint::logout, a, {{"biliCSRF", csrf->second}}, {});
                auto j = nlohmann::json::parse(r.body, nullptr, false);
                revoked = r.status == 200 && j.is_object() && (j.value("code",-1) == 0 || j.value("code",-1) == -101);
            }
            if (revoked && saveAccount(a.generation, {})) {
                brls::sync([] { quit(); });
                return;
            }
        } catch (const std::exception&) {}
        brls::sync([a] {
            auto* dialog = new brls::Dialog("退出尚未完成。可返回后重试。仅退出本机会移除设备凭据，服务端会话可能仍然有效。");
            dialog->addButton("返回", [] {});
            dialog->addButton("仅退出本机", [a] {
                try { if (saveAccount(a.generation, {})) quit(); }
                catch (const std::exception&) { brls::Application::notify("清除本机凭据失败"); }
            });
            dialog->open();
        });
    });
}
}
