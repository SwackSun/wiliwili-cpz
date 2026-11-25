// SPDX-License-Identifier: GPL-3.0-only
#include <cmath>
#include "native-video.hpp"
#include "danmaku-strip.hpp"
#include <xf86drm.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <algorithm>
#include <arm_neon.h>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <map>
#include <string>
#include <sys/mman.h>
#include <unistd.h>

namespace cm0 {
namespace {
struct Buffer {
    int fd;
    uint32_t id = 0, handle = 0, width, height, format;
    uint32_t pitch[4]{}, offset[4]{};
    uint64_t size = 0;
    uint8_t* data = nullptr;
    Buffer(int fd, uint32_t w, uint32_t h, uint32_t fmt) : fd(fd), width(w), height(h), format(fmt) {}
    ~Buffer() {
        if (id)
            drmModeRmFB(fd, id);
        if (data)
            munmap(data, size);
        if (handle) {
            drm_mode_destroy_dumb request{};
            request.handle = handle;
            drmIoctl(fd, DRM_IOCTL_MODE_DESTROY_DUMB, &request);
        }
    }
    bool configure(uint32_t w, uint32_t h) {
        if (id && width == w && height == h)
            return true;
        const bool rgb = format != DRM_FORMAT_NV12;
        const unsigned bytes = format == DRM_FORMAT_ARGB4444 ? 2 : (rgb ? 4 : 1);
        pitch[0] = (w * bytes + 63) & ~63u;
        pitch[1] = rgb ? 0 : pitch[0];
        offset[1] = rgb ? 0 : pitch[0] * h;
        if (uint64_t(pitch[0]) * h + uint64_t(pitch[1]) * (h / 2) > size)
            return false;
        uint32_t handles[4]{handle, rgb ? 0u : handle};
        uint32_t next = 0;
        if (drmModeAddFB2(fd, w, h, format, handles, pitch, offset, &next, 0))
            return false;
        if (id)
            drmModeRmFB(fd, id);
        id = next;
        width = w;
        height = h;
        return true;
    }
    bool allocate() {
        const bool rgb = format != DRM_FORMAT_NV12;
        drm_mode_create_dumb create{};
        // Reserve scanout memory before Mesa loads UI textures. Leave enough
        // room for the aligned chroma rows of a portrait 720x1280 stream.
        create.width = rgb ? width : 1280;
        create.height = rgb ? height : 1152;
        create.bpp = format == DRM_FORMAT_ARGB4444 ? 16 : (rgb ? 32 : 8);
        if (drmIoctl(fd, DRM_IOCTL_MODE_CREATE_DUMB, &create))
            return false;
        handle = create.handle;
        size = create.size;
        drm_mode_map_dumb map{};
        map.handle = handle;
        if (drmIoctl(fd, DRM_IOCTL_MODE_MAP_DUMB, &map))
            return false;
        void* ptr = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, map.offset);
        if (ptr == MAP_FAILED)
            return false;
        data = static_cast<uint8_t*>(ptr);
        return configure(width, height);
    }
    void copy(int plane, const uint8_t* source, int stride, int bytes, int rows) {
        auto* target = data + offset[plane];
        if (bytes == stride && uint32_t(bytes) == pitch[plane]) {
            std::memcpy(target, source, size_t(bytes) * rows);
        } else {
            for (int y = 0; y < rows; ++y) {
                std::memcpy(target, source, bytes);
                target += pitch[plane];
                source += stride;
            }
        }
    }
};
struct Plane {
    uint32_t id = 0;
    uint64_t z = 0;
    std::map<std::string, uint32_t> props;
    std::map<std::string, uint64_t> enums;
    std::vector<uint32_t> formats;
    bool supports(uint32_t fmt) const {
        return std::find(formats.begin(), formats.end(), fmt) != formats.end();
    }
    bool set(drmModeAtomicReq* req, const char* name, uint64_t value) const {
        auto it = props.find(name);
        return it != props.end() && drmModeAtomicAddProperty(req, id, it->second, value) >= 0;
    }
    bool region(drmModeAtomicReq* req, uint32_t crtc, uint32_t fb, const std::array<int, 4>& src,
                const std::array<int, 4>& dst) const {
        return set(req, "CRTC_ID", crtc) && set(req, "FB_ID", fb) &&
               set(req, "SRC_X", uint64_t(src[0]) << 16) && set(req, "SRC_Y", uint64_t(src[1]) << 16) &&
               set(req, "SRC_W", uint64_t(src[2] - src[0]) << 16) &&
               set(req, "SRC_H", uint64_t(src[3] - src[1]) << 16) && set(req, "CRTC_X", dst[0]) &&
               set(req, "CRTC_Y", dst[1]) && set(req, "CRTC_W", dst[2] - dst[0]) &&
               set(req, "CRTC_H", dst[3] - dst[1]);
    }
    bool disable(drmModeAtomicReq* req) const {
        return set(req, "FB_ID", 0) && set(req, "CRTC_ID", 0);
    }
};
struct Pair {
    std::unique_ptr<Buffer> buffers[2];
    unsigned displayed = 0;
    bool pending = false;
    bool reserve(int fd, int w, int h, uint32_t fmt) {
        for (auto& b : buffers) {
            b = std::make_unique<Buffer>(fd, w, h, fmt);
            if (!b->allocate())
                return false;
        }
        return true;
    }
    Buffer* next(int w, int h) {
        auto& b = buffers[1 - displayed];
        if (!b || !b->configure(w, h))
            return nullptr;
        pending = true;
        return b.get();
    }
    Buffer* current() { return buffers[pending ? 1 - displayed : displayed].get(); }
    void finish() {
        if (pending)
            displayed = 1 - displayed;
        pending = false;
    }
};
} // namespace
struct NativeVideo::State {
    int fd = -1, width = 0, height = 0;
    uint32_t crtc = 0;
    Plane video, ui, danmaku;
    Pair images, overlays, strips;
    std::unique_ptr<DanmakuStrip> comments;
    bool commentsRequested = false, commentsVisible = false;
    std::array<int, 4> src{}, dst{};
    bool bt709 = false, fullRange = false, visible = false;
    std::vector<uint32_t> previous;
    bool connect(int card, int uiWidth, int uiHeight) {
        fd = ::open(("/dev/dri/card" + std::to_string(card)).c_str(), O_RDWR | O_CLOEXEC);
        if (fd < 0 || drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1) || !drmIsMaster(fd))
            return false;
        auto* res = drmModeGetResources(fd);
        if (!res)
            return false;
        unsigned mask = 0;
        for (int i = 0; i < res->count_connectors && !crtc; ++i) {
            auto* conn = drmModeGetConnector(fd, res->connectors[i]);
            if (!conn)
                continue;
            if (conn->connector_type == DRM_MODE_CONNECTOR_HDMIA && conn->connection == DRM_MODE_CONNECTED) {
                auto* enc = drmModeGetEncoder(fd, conn->encoder_id);
                auto* active = enc ? drmModeGetCrtc(fd, enc->crtc_id) : nullptr;
                if (active && active->mode_valid) {
                    crtc = active->crtc_id;
                    width = active->mode.hdisplay;
                    height = active->mode.vdisplay;
                    for (int j = 0; j < res->count_crtcs; ++j)
                        if (res->crtcs[j] == crtc)
                            mask = 1u << j;
                }
                if (active)
                    drmModeFreeCrtc(active);
                if (enc)
                    drmModeFreeEncoder(enc);
            }
            drmModeFreeConnector(conn);
        }
        drmModeFreeResources(res);
        if (!mask)
            return false;
        auto* list = drmModeGetPlaneResources(fd);
        if (!list)
            return false;
        std::vector<Plane> available;
        uint64_t primaryZ = 0;
        for (unsigned i = 0; i < list->count_planes; ++i) {
            auto* plane = drmModeGetPlane(fd, list->planes[i]);
            if (!plane)
                continue;
            if (plane->possible_crtcs & mask) {
                Plane p;
                p.id = plane->plane_id;
                p.formats.assign(plane->formats, plane->formats + plane->count_formats);
                uint64_t type = DRM_PLANE_TYPE_CURSOR;
                auto* props = drmModeObjectGetProperties(fd, p.id, DRM_MODE_OBJECT_PLANE);
                for (unsigned j = 0; props && j < props->count_props; ++j) {
                    auto* prop = drmModeGetProperty(fd, props->props[j]);
                    if (!prop)
                        continue;
                    p.props[prop->name] = prop->prop_id;
                    if (!strcmp(prop->name, "type"))
                        type = props->prop_values[j];
                    if (!strcmp(prop->name, "zpos"))
                        p.z = props->prop_values[j];
                    for (int k = 0; k < prop->count_enums; ++k)
                        p.enums[prop->enums[k].name] = prop->enums[k].value;
                    drmModeFreeProperty(prop);
                }
                if (props)
                    drmModeFreeObjectProperties(props);
                if (type == DRM_PLANE_TYPE_PRIMARY && plane->crtc_id == crtc)
                    primaryZ = p.z;
                if (type == DRM_PLANE_TYPE_OVERLAY && !plane->crtc_id)
                    available.push_back(std::move(p));
            }
            drmModeFreePlane(plane);
        }
        drmModeFreePlaneResources(list);
        std::sort(available.begin(), available.end(), [](const Plane& a, const Plane& b) { return a.z < b.z; });
        for (const auto& p : available) {
            if (!video.id && p.z > primaryZ && p.supports(DRM_FORMAT_NV12))
                video = p;
            else if (video.id && p.z > video.z && p.supports(DRM_FORMAT_ARGB8888)) {
                if (!ui.id) {
                    ui = p;
                } else if (p.z > ui.z && ui.supports(DRM_FORMAT_ARGB4444)) {
                    danmaku = ui;
                    ui = p;
                    break;
                }
            }
        }
        return ui.id && images.reserve(fd, 1280, 720, DRM_FORMAT_NV12) &&
               overlays.reserve(fd, uiWidth, uiHeight, DRM_FORMAT_ARGB8888);
    }
    ~State() {
        images = {};
        overlays = {};
        strips = {};
        if (fd >= 0)
            close(fd);
    }
};
NativeVideo::NativeVideo() : state(std::make_unique<State>()) {}
std::unique_ptr<NativeVideo> NativeVideo::open(int uiWidth, int uiHeight) {
    for (int i = 0; i < 4; ++i) {
        auto device = std::unique_ptr<NativeVideo>(new NativeVideo);
        if (device->state->connect(i, uiWidth, uiHeight))
            return device;
    }
    return {};
}
NativeVideo::~NativeVideo() { hide(); }
DanmakuStrip* NativeVideo::requestDanmaku() {
    auto& s = *state;
    s.commentsRequested = s.danmaku.id && !s.comments;
    return s.comments.get();
}

void NativeVideo::hide() {
    auto& s = *state;
    if (!s.visible)
        return;
    auto* req = drmModeAtomicAlloc();
    if (req) {
        s.video.disable(req);
        s.ui.disable(req);
        if (s.commentsVisible) s.danmaku.disable(req);
        drmModeAtomicCommit(s.fd, req, 0, nullptr);
        drmModeAtomicFree(req);
    }
    s.visible = false;
    s.commentsVisible = s.commentsRequested = false;
    s.comments.reset();
    s.strips = {};
}
// Blend the displayed NV12 plane under the premultiplied ARGB UI only for PrintScreen.
void NativeVideo::composeScreenshot(std::vector<uint32_t>& ui, int w, int h) const {
    auto& s = *state;
    auto* b = s.images.buffers[s.images.displayed].get();
    if (!s.visible || !b || !b->data) return;
    if (s.commentsVisible) {
        const auto& strip = *s.strips.buffers[s.strips.displayed];
        for (int y = 0; y < h && y * 720 / h < int(strip.height); ++y) {
            const auto* line = reinterpret_cast<const uint16_t*>(strip.data + (y * 720 / h) * strip.pitch[0]);
            for (int x = 0; x < w; ++x) {
                const uint32_t comment = line[x * strip.width / w];
                if (!comment) continue;
                auto& pixel = ui[y * w + x];
                const unsigned clear = 255 - (pixel >> 24);
                pixel += (((((comment >> 12) & 15) * 17 * clear / 255) << 24) |
                          ((((comment >> 8) & 15) * 17 * clear / 255) << 16) |
                          ((((comment >> 4) & 15) * 17 * clear / 255) << 8) |
                          ((comment & 15) * 17 * clear / 255));
            }
        }
    }
    int x0 = std::lround(viewport[0]*w), y0 = std::lround(viewport[1]*h);
    int x1 = std::lround(viewport[2]*w), y1 = std::lround(viewport[3]*h);
    for (int y = std::max(0,y0); y < std::min(h,y1); ++y) {
        int sy = std::clamp(s.src[1]+(y-y0)*(s.src[3]-s.src[1])/(y1-y0), 0, int(b->height)-1);
        for (int x = std::max(0,x0); x < std::min(w,x1); ++x) {
            auto& pixel = ui[size_t(y)*w+x];
            int alpha = pixel >> 24;
            if (alpha == 255) continue;
            int sx = std::clamp(s.src[0]+(x-x0)*(s.src[2]-s.src[0])/(x1-x0), 0, int(b->width)-1);
            float luma = b->data[sy*b->pitch[0]+sx];
            auto* uv = b->data+b->offset[1]+(sy/2)*b->pitch[1]+(sx & ~1);
            float u = int(uv[0])-128, v = int(uv[1])-128;
            if (!s.fullRange) { luma = (luma-16)*255/219; u *= 255.f/224; v *= 255.f/224; }
            auto blend = [&](int shift, float color) -> uint32_t {
                int fg = (pixel >> shift) & 255;
                return std::min(255, fg + (255-alpha)*std::clamp(int(std::lround(color)),0,255)/255);
            };
            auto r = blend(16,luma+(s.bt709 ? 1.5748f : 1.402f)*v);
            auto g = blend(8,luma-(s.bt709 ? .1873f : .3441f)*u-(s.bt709 ? .4681f : .7141f)*v);
            auto blue = blend(0,luma+(s.bt709 ? 1.8556f : 1.772f)*u);
            pixel = 0xff000000u | (r<<16) | (g<<8) | blue;
        }
    }
}
bool NativeVideo::upload(const mpv_cm0_frame& f, int w, int h) {
    auto& s = *state;
    if (w <= 0 || h <= 0 || f.width <= 0 || f.height <= 0 || std::max(f.width, f.height) > 1280 ||
        std::min(f.width, f.height) > 720 || (f.width & 1) || (f.height & 1))
        return false;
    // VC4 accounts each YUV plane separately in its scanout clock budget.
    // Interleaved chroma fits alongside fbcon and the fullscreen ARGB controls at 1080p60.
    auto* b = s.images.next(f.width, f.height);
    if (!b)
        return false;
    b->copy(0, f.planes[0], f.stride[0], f.width, f.height);
    if (f.nv12) {
        b->copy(1, f.planes[1], f.stride[1], f.width, f.height / 2);
    } else {
        for (int y = 0; y < f.height / 2; ++y) {
            auto* dst = b->data + b->offset[1] + y * b->pitch[1];
            auto* u = f.planes[1] + y * f.stride[1];
            auto* v = f.planes[2] + y * f.stride[2];
            int x = 0;
            for (; x + 16 <= f.width / 2; x += 16) {
                uint8x16x2_t uv{{vld1q_u8(u + x), vld1q_u8(v + x)}};
                vst2q_u8(dst + 2 * x, uv);
            }
            for (; x < f.width / 2; ++x) {
                dst[2 * x] = u[x];
                dst[2 * x + 1] = v[x];
            }
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        s.src[i] = f.src[i];
        viewport[i] = float(f.dst[i]) / (i % 2 ? h : w);
        s.dst[i] = (int64_t(f.dst[i]) * (i % 2 ? s.height : s.width) + (i % 2 ? h : w) / 2) / (i % 2 ? h : w);
    }
    s.bt709 = f.bt709;
    s.fullRange = f.full_range;
    return true;
}
bool NativeVideo::show(const std::vector<uint32_t>& pixels, int w, int h) {
    auto& s = *state;
    auto* b = s.images.current();
    if (!b)
        return false;
    if (pixels != s.previous) {
        auto* overlay = s.overlays.next(w, h);
        if (!overlay)
            return false;
        overlay->copy(0, reinterpret_cast<const uint8_t*>(pixels.data()), w * 4, w * 4, h);
        s.previous = pixels;
    }
    const bool drawComments = s.comments && s.comments->active();
    if (!s.images.pending && !s.overlays.pending && s.visible && !drawComments &&
        !s.commentsVisible && !s.commentsRequested)
        return true;
    auto* req = drmModeAtomicAlloc();
    if (!req)
        return false;
    bool ok = s.video.region(req, s.crtc, b->id, s.src, s.dst) &&
              s.ui.region(req, s.crtc, s.overlays.current()->id, {0, 0, w, h}, {0, 0, s.width, s.height});
    for (const auto& [prop, name] :
         {std::pair{"COLOR_ENCODING", s.bt709 ? "ITU-R BT.709 YCbCr" : "ITU-R BT.601 YCbCr"},
          std::pair{"COLOR_RANGE", s.fullRange ? "YCbCr full range" : "YCbCr limited range"}}) {
        auto it = s.video.enums.find(name);
        if (it != s.video.enums.end())
            ok = ok && s.video.set(req, prop, it->second);
    }
    const std::array<int, 4> stripSrc{0, 0, DanmakuStrip::width, DanmakuStrip::height};
    const std::array<int, 4> stripDst{0, 0, s.width, s.height * DanmakuStrip::height / 720};
    if (ok && s.commentsRequested && !s.comments) {
        s.comments = std::make_unique<DanmakuStrip>();
        bool supported = s.comments->ready() &&
            s.strips.reserve(s.fd, DanmakuStrip::width, DanmakuStrip::height, DRM_FORMAT_ARGB4444);
        if (supported) {
            auto* test = drmModeAtomicDuplicate(req);
            supported = test && s.danmaku.region(test, s.crtc, s.strips.current()->id, stripSrc, stripDst) &&
                drmModeAtomicCommit(s.fd, test, DRM_MODE_ATOMIC_TEST_ONLY, nullptr) == 0;
            if (test) drmModeAtomicFree(test);
        }
        if (!supported) {
            s.comments.reset();
            s.strips = {};
            s.danmaku.id = 0;
        }
        std::cerr << "HDMI danmaku strip " << (supported ? "1280x192 ARGB4444" : "unavailable") << '\n';
    }
    s.commentsRequested = false;
    const bool baseReady = ok;
    const int baseProperties = drmModeAtomicGetCursor(req);
    if (ok && drawComments) {
        auto* strip = s.strips.next(DanmakuStrip::width, DanmakuStrip::height);
        ok = strip && s.danmaku.region(req, s.crtc, strip->id, stripSrc, stripDst);
        if (ok) s.comments->copyTo(1 - s.strips.displayed, strip->data, strip->pitch[0]);
        s.comments->clear();
    } else if (ok && s.commentsVisible) {
        ok = s.danmaku.disable(req);
    }
    // A blocking atomic commit completes the flip before the back page is reused.
    bool committed = ok && drmModeAtomicCommit(s.fd, req, 0, nullptr) == 0;
    bool stripDisplayed = drawComments;
    if (!committed && baseReady && drawComments) {
        drmModeAtomicSetCursor(req, baseProperties);
        committed = (!s.commentsVisible || s.danmaku.disable(req)) &&
            drmModeAtomicCommit(s.fd, req, 0, nullptr) == 0;
        if (committed) {
            s.comments.reset();
            s.strips = {};
            s.danmaku.id = 0;
            stripDisplayed = false;
        }
    }
    drmModeAtomicFree(req);
    if (!committed) {
        std::cerr << "HDMI plane commit failed: " << strerror(errno) << '\n';
        return false;
    }
    if (!s.visible)
        std::cerr << "HDMI video plane " << b->width << 'x' << b->height << " on " << s.width << 'x' << s.height
                  << '\n';
    s.visible = true;
    s.commentsVisible = stripDisplayed;
    s.images.finish();
    s.overlays.finish();
    s.strips.finish();
    return true;
}
} // namespace cm0
