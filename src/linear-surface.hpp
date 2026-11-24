// SPDX-License-Identifier: GPL-3.0-only
#pragma once
#include <EGL/egl.h>
#include <GLES2/gl2.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace cm0 {
// A linear GLES color attachment shared with the framebuffer presenter.
class LinearSurface {
public:
    static std::unique_ptr<LinearSurface> create(int width, int height);
    ~LinearSurface();
    bool read(std::vector<uint32_t>& rgb) const;
    LinearSurface(const LinearSurface&) = delete;
    LinearSurface& operator=(const LinearSurface&) = delete;
private:
    LinearSurface(int width, int height);
    bool attach();
    int width_, height_, stride_;
    size_t length_;
    int fd_ = -1;
    void* mapping_;
    EGLContext context_;
    GLuint fbo_ = 0, color_ = 0, stencil_ = 0;
};
}
