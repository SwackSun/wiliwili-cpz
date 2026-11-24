// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "account-store.hpp"
#include <functional>

namespace cm0 {
enum class AccountEndpoint { status, correspond, refresh, confirm, logout };
struct AccountReply {
    int status = 0;
    std::string body;
    CookieMap cookies;
    std::map<std::string, int64_t> expires;
};
using AccountRequest = std::function<AccountReply(AccountEndpoint, const Account&, const CookieMap&, const std::string&)>;
bool synchronize(Account account, const AccountRequest& request);
}
