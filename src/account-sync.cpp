// SPDX-License-Identifier: GPL-3.0-only
#include "account-sync.hpp"
#include <nlohmann/json.hpp>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <memory>
#include <stdexcept>

namespace cm0 {
namespace {
using Json = nlohmann::json;
std::string correspondPath(int64_t timestamp) {
    constexpr char key[] = "-----BEGIN PUBLIC KEY-----\n"
        "MIGfMA0GCSqGSIb3DQEBAQUAA4GNADCBiQKBgQDLgd2OAkcGVtoE3ThUREbio0Eg\n"
        "Uc/prcajMKXvkCKFCWhJYJcLkcM2DKKcSeFpD/j6Boy538YXnR6VhcuUJOhH2x71\n"
        "nzPjfdTcqMz7djHum0qSZA0AyCBDABUqCrfNgCiJ00Ra7GmRj+YCK1NJEuewlb40\n"
        "JNrRuoEUXpabUzGB8QIDAQAB\n-----END PUBLIC KEY-----\n";
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(BIO_new_mem_buf(key, -1), BIO_free);
    if (!bio) return {};
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> pub(PEM_read_bio_PUBKEY(bio.get(), nullptr, nullptr, nullptr), EVP_PKEY_free);
    if (!pub) return {};
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> ctx(EVP_PKEY_CTX_new(pub.get(), nullptr), EVP_PKEY_CTX_free);
    if (!ctx || EVP_PKEY_encrypt_init(ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_padding(ctx.get(), RSA_PKCS1_OAEP_PADDING) <= 0 ||
        EVP_PKEY_CTX_set_rsa_oaep_md(ctx.get(), EVP_sha256()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_mgf1_md(ctx.get(), EVP_sha256()) <= 0) return {};
    std::string message = "refresh_" + std::to_string(timestamp);
    unsigned char encrypted[128];
    size_t size = sizeof(encrypted);
    if (EVP_PKEY_encrypt(ctx.get(), encrypted, &size, reinterpret_cast<const unsigned char*>(message.data()), message.size()) <= 0) return {};
    constexpr char hex[] = "0123456789abcdef";
    std::string out;
    for (size_t i = 0; i < size; ++i) { out += hex[encrypted[i] >> 4]; out += hex[encrypted[i] & 15]; }
    return out;
}

Json jsonReply(const AccountReply& r) {
    if (r.status != 200) return {};
    return Json::parse(r.body, nullptr, false);
}
bool confirm(Account& a, const AccountRequest& request) {
    auto reply = jsonReply(request(AccountEndpoint::confirm, a, {{"csrf", a.cookies.at("bili_jct")},
                             {"refresh_token", a.pendingConfirmation}}, {}));
    if (!reply.is_object() || reply.value("code", -1) != 0) return false;
    a.pendingConfirmation.clear();
    return saveAccount(a.generation, a);
}
}

bool synchronize(Account account, const AccountRequest& request) {
    auto reply = jsonReply(request(AccountEndpoint::status, account, {}, {}));
    if (!reply.is_object()) return false;
    int code = reply.at("code").get<int>();
    if (code == -101) return saveAccount(account.generation, {});
    if (code != 0) return false;
    if (!account.pendingConfirmation.empty()) return confirm(account, request);
    const auto& data = reply.at("data");
    if (!data.at("refresh").get<bool>()) return true;
    if (account.refresh.empty()) return false;
    auto path = correspondPath(data.at("timestamp").get<int64_t>());
    if (path.empty()) return false;
    auto page = request(AccountEndpoint::correspond, account, {}, path);
    if (page.status != 200) return false;
    constexpr std::string_view marker = "id=\"1-name\">";
    size_t start = page.body.find(marker);
    if (start == std::string::npos) return false;
    start += marker.size();
    auto end = page.body.find('<', start);
    if (end == std::string::npos || end == start || end - start > 256) return false;
    auto rotated = request(AccountEndpoint::refresh, account, {{"csrf", account.cookies.at("bili_jct")},
        {"refresh_csrf",page.body.substr(start, end-start)}, {"source","main_web"}, {"refresh_token",account.refresh}}, {});
    auto result = jsonReply(rotated);
    if (!result.is_object() || result.value("code",-1) != 0) return false;
    auto token = result.at("data").at("refresh_token").get<std::string>();
    if (token.empty() || rotated.cookies["SESSDATA"].empty() || rotated.cookies["bili_jct"].empty()) return false;
    account.pendingConfirmation = account.refresh;
    account.refresh = std::move(token);
    for (const auto& item : rotated.cookies) {
        account.cookies[item.first] = item.second;
        account.expires[item.first] = rotated.expires[item.first];
    }
    if (!saveAccount(account.generation, account)) return false;
    ++account.generation;
    return confirm(account, request);
}
}
