// SPDX-License-Identifier: GPL-3.0-only
#include "account-store.hpp"
#include "private-settings.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/crypto.h>
#include <nlohmann/json.hpp>
#include <array>
#include <ctime>
#include <sys/file.h>
#include <filesystem>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <sys/stat.h>

namespace cm0 {
namespace {
std::mutex mutex;
Account active;
std::string vaultPath, keyPath;
constexpr size_t maxBytes = 128 * 1024;
constexpr char version[] = "WILI1";
struct Key {
    std::array<unsigned char, 32> bytes{};
    ~Key() { OPENSSL_cleanse(bytes.data(), bytes.size()); }
};
void require(bool ok) {
    if (!ok) throw std::runtime_error("Cannot securely read or write wiliwili account");
}
bool readPrivate(const std::string& path, std::string& out) {
    int fd = open(path.c_str(), O_RDONLY | O_NOFOLLOW | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0 && errno == ENOENT) return false;
    require(fd >= 0);
    struct stat st{};
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_uid == geteuid() &&
              !(st.st_mode & 077) && st.st_size > 0 && st.st_size <= static_cast<off_t>(maxBytes);
    if (ok) {
        out.resize(st.st_size);
        size_t offset = 0;
        while (offset < out.size()) {
            auto n = read(fd, out.data() + offset, out.size() - offset);
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0) break;
            offset += n;
        }
        ok = offset == out.size();
    }
    close(fd);
    require(ok);
    return true;
}
void loadKey(Key& key, bool create) {
    std::string bytes;
    if (!readPrivate(keyPath, bytes)) {
        require(create);
        std::filesystem::create_directories(std::filesystem::path(keyPath).parent_path());
        require(RAND_bytes(key.bytes.data(), key.bytes.size()) == 1);
        // Publish only a complete key, without replacing a concurrent creator's key.
        std::string tmp = keyPath + ".XXXXXX";
        int fd = mkstemp(tmp.data());
        require(fd >= 0);
        bool ok = write(fd, key.bytes.data(), key.bytes.size()) == ssize_t(key.bytes.size()) && fsync(fd) == 0;
        if (close(fd) != 0) ok = false;
        if (ok) ok = link(tmp.c_str(), keyPath.c_str()) == 0 || errno == EEXIST;
        unlink(tmp.c_str());
        require(ok && syncParent(keyPath) && readPrivate(keyPath, bytes));
    }
    require(bytes.size() == key.bytes.size());
    std::copy(bytes.begin(), bytes.end(), key.bytes.begin());
    OPENSSL_cleanse(bytes.data(), bytes.size());
}

std::string crypt(const std::string& input, bool encrypt) {
    constexpr int prefix = sizeof(version) - 1, nonce = 12, tag = 16, header = prefix + nonce + tag;
    require(input.size() <= maxBytes - (encrypt ? header : 0));
    Key key;
    loadKey(key, encrypt);
    std::string envelope = encrypt ? std::string(header, '\0') : input;
    require(encrypt || (envelope.size() >= header && envelope.compare(0, prefix, version) == 0));
    auto* iv = reinterpret_cast<unsigned char*>(envelope.data() + prefix);
    if (encrypt) {
        envelope.replace(0, prefix, version);
        iv = reinterpret_cast<unsigned char*>(envelope.data() + prefix);
        require(RAND_bytes(iv, nonce) == 1);
    }
    std::unique_ptr<EVP_CIPHER_CTX, decltype(&EVP_CIPHER_CTX_free)> ctx(EVP_CIPHER_CTX_new(), EVP_CIPHER_CTX_free);
    require(ctx && EVP_CipherInit_ex(ctx.get(), EVP_aes_256_gcm(), nullptr, key.bytes.data(), iv, encrypt) == 1);
    auto* auth = iv + nonce;
    if (!encrypt) require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_SET_TAG, tag, auth) == 1);
    size_t start = encrypt ? 0 : header;
    std::string output(input.size() - start + EVP_MAX_BLOCK_LENGTH, '\0');
    auto* dest = reinterpret_cast<unsigned char*>(output.data());
    int written = 0, tail = 0;
    require(EVP_CipherUpdate(ctx.get(), dest, &written,
        reinterpret_cast<const unsigned char*>(input.data() + start), input.size() - start) == 1);
    require(EVP_CipherFinal_ex(ctx.get(), dest + written, &tail) == 1);
    output.resize(written + tail);
    if (!encrypt) return output;
    require(EVP_CIPHER_CTX_ctrl(ctx.get(), EVP_CTRL_GCM_GET_TAG, tag, auth) == 1);
    return envelope + output;
}
class DirectoryLock {
public:
    DirectoryLock() {
        auto parent = std::filesystem::path(vaultPath).parent_path();
        std::filesystem::create_directories(parent);
        fd = open(parent.c_str(), O_DIRECTORY | O_RDONLY | O_CLOEXEC);
        require(fd >= 0);
        while (flock(fd, LOCK_EX) < 0) {
            if (errno == EINTR) continue;
            close(fd);
            require(false);
        }
    }
    ~DirectoryLock() { close(fd); }
    DirectoryLock(const DirectoryLock&) = delete;
private:
    int fd;
};

std::string cachedEnvelope;
bool reload() {
    std::string envelope;
    if (!readPrivate(vaultPath, envelope)) {
        cachedEnvelope.clear();
        active = {};
        return false;
    }
    if (envelope == cachedEnvelope) return true;
    auto j = nlohmann::json::parse(crypt(envelope, false));
    Account disk;
    j.at("cookie").get_to(disk.cookies);
    j.at("expires").get_to(disk.expires);
    j.at("refresh").get_to(disk.refresh);
    j.at("pending").get_to(disk.pendingConfirmation);
    j.at("revision").get_to(disk.generation);
    active = std::move(disk);
    cachedEnvelope = std::move(envelope);
    return true;
}
void persist(Account next) {
    ++next.generation;
    nlohmann::json record{{"cookie", next.cookies}, {"expires", next.expires}, {"refresh", next.refresh},
                          {"pending", next.pendingConfirmation}, {"revision", next.generation}};
    auto envelope = crypt(record.dump(), true);
    require(writeSettings(vaultPath, envelope));
    cachedEnvelope = std::move(envelope);
    active = std::move(next);
}
}
Account loadAccount(const std::string& configDir, const std::string& dataDir, Account legacy) {
    std::lock_guard<std::mutex> guard(mutex);
    vaultPath = configDir + "/account.enc";
    keyPath = dataDir + "/wiliwili/account.key";
    cachedEnvelope.clear();
    active = {};
    DirectoryLock lock;
    if (!reload() && (legacy.cookies.count("SESSDATA") || !legacy.refresh.empty())) {
        legacy.generation = 0;
        persist(std::move(legacy));
    }
    return active;
}
Account currentAccount() {
    std::lock_guard<std::mutex> guard(mutex);
    // Atomic replacement lets readers see only whole records. Reuse decrypted data when unchanged.
    if (!vaultPath.empty()) reload();
    return active;
}
bool saveAccount(uint64_t generation, Account next) {
    std::lock_guard<std::mutex> guard(mutex);
    DirectoryLock lock;
    reload();
    if (active.generation != generation) return false;
    next.generation = generation;
    persist(std::move(next));
    return true;
}
CookieMap validCookies(const Account& account) {
    auto result = account.cookies;
    const auto now = std::time(nullptr);
    for (const auto& item : account.expires) {
        if (item.second > 0 && item.second <= now) result.erase(item.first);
    }
    auto session = result.find("SESSDATA");
    if (session == result.end() || session->second.empty()) return {};
    return result;
}

}
