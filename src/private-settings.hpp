// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <cerrno>
#include <cstdlib>
#include <cstdio>
#include <fcntl.h>
#include <string>
#include <unistd.h>

namespace cm0 {
inline bool syncParent(const std::string& path) {
    auto slash = path.find_last_of('/');
    auto parent = slash == path.npos ? "." : path.substr(0, slash);
    int fd = open(parent.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (fd < 0) return false;
    bool ok = fsync(fd) == 0;
    close(fd);
    return ok;
}
// Rename a private sibling only after all bytes are durable. Preserve the old file on failure.
inline bool writeSettings(const std::string& path, const std::string& bytes) {
    std::string temp = path + ".XXXXXX";
    int fd = mkstemp(temp.data());
    if (fd < 0) return false;
    size_t offset = 0;
    while (offset < bytes.size()) {
        const auto n = write(fd, bytes.data() + offset, bytes.size() - offset);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) break;
        offset += static_cast<size_t>(n);
    }
    bool ok = offset == bytes.size() && fsync(fd) == 0;
    if (close(fd) != 0) ok = false;
    if (ok && rename(temp.c_str(), path.c_str()) == 0) return syncParent(path);
    unlink(temp.c_str());
    return false;
}
}
