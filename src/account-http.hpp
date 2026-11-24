// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include "account-store.hpp"
#include <cpr/cookies.h>

namespace cm0 {
void selectLoginKey(const std::string& key);
void cancelLoginKey(const std::string& key);
bool acceptLogin(uint64_t generation, const cpr::Cookies& response, const std::string& refresh, CookieMap cookies,
                 const std::string& key = {});
void checkAccount();
}
