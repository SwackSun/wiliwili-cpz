// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cstdint>
#include <map>
#include <string>

namespace cm0 {
using CookieMap = std::map<std::string, std::string>;
struct Account {
    CookieMap cookies;
    std::map<std::string, int64_t> expires;
    std::string refresh, pendingConfirmation;
    uint64_t generation = 0;
};

Account loadAccount(const std::string& configDir, const std::string& dataDir, Account legacy);
Account currentAccount();
CookieMap validCookies(const Account& account);
bool saveAccount(uint64_t generation, Account next);
}
