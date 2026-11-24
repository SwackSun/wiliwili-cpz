// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <string_view>

namespace cm0 {
inline bool accountApi(std::string_view url) {
    // Fixed API origins only. Resource URLs use a separate anonymous session.
    constexpr std::string_view scheme = "https://";
    if (url.substr(0, scheme.size()) != scheme) return false;
    for (unsigned char c : url) {
        if (c <= 32 || c == 127 || c == '\\') return false;
    }
    url.remove_prefix(scheme.size());
    const auto slash = url.find('/');
    if (slash == url.npos) return false;
    const auto host = url.substr(0, slash);
    const auto path = url.substr(slash);
    if (host == "passport.bilibili.com") return path == "/login/exit/v2";
    if (path.substr(0, path.find_first_of("?#")) == "/x/click-interface/click/now") return false;
    return host == "api.bilibili.com" || host == "app.bilibili.com" ||
           host == "api.vc.bilibili.com" || host == "api.live.bilibili.com" ||
           host == "bangumi.bilibili.com";
}
inline constexpr char clockUrl[] = "https://api.bilibili.com/x/click-interface/click/now";
inline constexpr char logoutUrl[] = "https://passport.bilibili.com/login/exit/v2";
inline constexpr char pollUrl[] = "https://passport.bilibili.com/x/passport-login/web/qrcode/poll";
}
