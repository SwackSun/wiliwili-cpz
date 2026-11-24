// SPDX-License-Identifier: GPL-3.0-only
#include "linear-surface.hpp"
#include <EGL/eglext.h>
#include <GLES2/gl2ext.h>
#include <libdrm/drm_fourcc.h>
#include <linux/dma-buf.h>
#include <linux/dma-heap.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <string_view>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace cm0 {
namespace {
bool hasExtension(const char* list, std::string_view name) {
    if (!list) return false;
    std::string_view extensions(list);
    while (!extensions.empty()) {
        auto end = extensions.find(' ');
        if (extensions.substr(0, end) == name) return true;
        if (end == extensions.npos) break;
        extensions.remove_prefix(end + 1);
    }
    return false;
}

bool syncBuffer(int fd, uint64_t flags) {
    dma_buf_sync access{flags | DMA_BUF_SYNC_READ};
    int status;
    do { status = ioctl(fd, DMA_BUF_IOCTL_SYNC, &access); }
    while (status < 0 && (errno == EINTR || errno == EAGAIN));
    return status == 0;
}
}

LinearSurface::LinearSurface(int width, int height)
    : width_(width), height_(height), stride_((width + 63) / 64 * 64),
      length_(size_t(stride_) * ((height + 63) / 64 * 64) * sizeof(uint32_t)),
      mapping_(MAP_FAILED), context_(eglGetCurrentContext()) {}

std::unique_ptr<LinearSurface> LinearSurface::create(int width, int height) {
    if (width <= 0 || width > 4096 || height <= 0 || height > 2160) return {};
    auto surface = std::unique_ptr<LinearSurface>(new LinearSurface(width, height));
    if (!surface->attach()) return {};
    return surface;
}

bool LinearSurface::attach() {
    auto display = eglGetCurrentDisplay();
    if (display == EGL_NO_DISPLAY || context_ == EGL_NO_CONTEXT) return false;
    const char* extensions = eglQueryString(display, EGL_EXTENSIONS);
    // Specify a linear modifier explicitly: a tiled import cannot be read as rows.
    if (!hasExtension(extensions, "EGL_EXT_image_dma_buf_import_modifiers")) return false;
    const auto* glExtensions = reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if (!hasExtension(glExtensions, "GL_OES_EGL_image")) return false;
    auto createImage = reinterpret_cast<PFNEGLCREATEIMAGEKHRPROC>(eglGetProcAddress("eglCreateImageKHR"));
    auto destroyImage = reinterpret_cast<PFNEGLDESTROYIMAGEKHRPROC>(eglGetProcAddress("eglDestroyImageKHR"));
    auto bindImage = reinterpret_cast<PFNGLEGLIMAGETARGETRENDERBUFFERSTORAGEOESPROC>(
        eglGetProcAddress("glEGLImageTargetRenderbufferStorageOES"));
    if (!createImage || !destroyImage || !bindImage) return false;

    int heap = open("/dev/dma_heap/default_cma_region", O_RDONLY | O_CLOEXEC);
    if (heap < 0) return false;
    dma_heap_allocation_data allocation{};
    allocation.len = length_;
    allocation.fd_flags = O_RDWR | O_CLOEXEC;
    const int result = ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &allocation);
    close(heap);
    if (result < 0) return false;
    fd_ = allocation.fd;
    mapping_ = mmap(nullptr, length_, PROT_READ, MAP_SHARED, fd_, 0);
    if (mapping_ == MAP_FAILED) return false;

    const EGLint attrs[] = {
        EGL_WIDTH, width_, EGL_HEIGHT, height_, EGL_LINUX_DRM_FOURCC_EXT, DRM_FORMAT_ARGB8888,
        EGL_DMA_BUF_PLANE0_FD_EXT, fd_, EGL_DMA_BUF_PLANE0_OFFSET_EXT, 0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT, stride_ * EGLint(sizeof(uint32_t)),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT, 0, EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT, 0, EGL_NONE
    };
    auto image = createImage(display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attrs);
    if (image == EGL_NO_IMAGE_KHR) return false;
    // Called on a newly-created context, before NanoVG initializes.
    glGenFramebuffers(1, &fbo_);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo_);
    glGenRenderbuffers(1, &color_);
    glBindRenderbuffer(GL_RENDERBUFFER, color_);
    bindImage(GL_RENDERBUFFER, image);
    destroyImage(display, image);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, color_);
    glGenRenderbuffers(1, &stencil_);
    glBindRenderbuffer(GL_RENDERBUFFER, stencil_);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, width_, height_);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, stencil_);
    glBindRenderbuffer(GL_RENDERBUFFER, 0);
    return glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE;
}

bool LinearSurface::read(std::vector<uint32_t>& rgb) const {
    if (rgb.size() != size_t(width_) * height_) return false;
    // Finish GPU writes before opening the CPU's cache-coherent read interval.
    glFinish();
    if (!syncBuffer(fd_, DMA_BUF_SYNC_START)) return false;
    const auto* src = static_cast<const uint32_t*>(mapping_);
    for (int y = 0; y < height_; ++y)
        std::memcpy(rgb.data() + y * width_, src + (height_ - 1 - y) * stride_, width_ * sizeof(uint32_t));
    return syncBuffer(fd_, DMA_BUF_SYNC_END);
}

LinearSurface::~LinearSurface() {
    if (context_ != EGL_NO_CONTEXT && context_ == eglGetCurrentContext()) {
        glDeleteFramebuffers(1, &fbo_);
        glDeleteRenderbuffers(1, &color_);
        glDeleteRenderbuffers(1, &stencil_);
    }
    if (mapping_ != MAP_FAILED) munmap(mapping_, length_);
    if (fd_ >= 0) close(fd_);
}
}
