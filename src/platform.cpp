// SPDX-License-Identifier: GPL-3.0-only
#include "device-runtime.hpp"
#include "danmaku-strip.hpp"
#include "linear-surface.hpp"
#include "lcd-search-input.hpp"
#include "native-video.hpp"
#include "playback-display.hpp"
#include <SDL2/SDL.h>
#include <GLES2/gl2.h>
#include "scancodes_linux.h"
#include <borealis/core/application.hpp>
#include "view/mpv_core.hpp"
#include "view/video_view.hpp"
#include "fragment/mine_qr_login.hpp"
#include "view/recycling_grid.hpp"
#include "utils/activity_helper.hpp"
#include <borealis/views/dialog.hpp>
#include "activity/main_activity.hpp"
#include "activity/setting_activity.hpp"
#include "activity/player_activity.hpp"
#include "activity/live_player_activity.hpp"
#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <exception>
#include <fstream>
#include <iostream>
#include <iterator>
#include <linux/input.h>
#include <malloc.h>
#include <memory>
#include <optional>
#include <cmath>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/stat.h>
#include <utility>
#include <vector>

int runClient(int, char**, const std::string&);

extern "C" {
struct SDL_VideoDevice;
EGLSurface __real_SDL_EGL_CreateOffscreenSurface(SDL_VideoDevice*, int, int);
void __real_SDL_SetWindowSize(SDL_Window*, int, int);
SDL_GLContext __real_SDL_GL_CreateContext(SDL_Window*);
SDL_Window* __real_SDL_CreateWindow(const char*, int, int, int, int, Uint32);
int __real_SDL_PollEvent(SDL_Event*);
void __real_SDL_GL_SwapWindow(SDL_Window*);
int __real_mpv_render_context_render(mpv_render_context*, mpv_render_param*);
void __real_nvgUpdateImage(NVGcontext*, int, const unsigned char*);
// These SDL internals belong to the pinned, statically linked SDL offscreen driver.
void SDL_SetKeyboardFocus(SDL_Window*);
int SDL_SendKeyboardKey(Uint8, SDL_Scancode);
int SDL_SendKeyboardText(const char*);
}

namespace {
using Clock = std::chrono::steady_clock;
int width = 320;
int height = 170;
struct PlaybackGain {
    double volume;
    int mute;
};

std::optional<PlaybackGain> readGain(mpv_handle* player) {
    PlaybackGain gain{};
    if (!player || mpv_get_property(player, "volume", MPV_FORMAT_DOUBLE, &gain.volume) < 0 ||
        mpv_get_property(player, "mute", MPV_FORMAT_FLAG, &gain.mute) < 0) return {};
    return gain;
}

void applyGain(mpv_handle* player, PlaybackGain gain) {
    if (gain.mute) mpv_set_property(player, "mute", MPV_FORMAT_FLAG, &gain.mute);
    mpv_set_property(player, "volume", MPV_FORMAT_DOUBLE, &gain.volume);
    if (!gain.mute) mpv_set_property(player, "mute", MPV_FORMAT_FLAG, &gain.mute);
}

struct Session {
    std::unique_ptr<cm0::Display> display;
    std::unique_ptr<cm0::Keyboard> keyboard;
    std::unique_ptr<cm0::LinearSurface> surface;
    std::vector<uint32_t> rgb;
    Clock::time_point nextProbe{};
    Clock::time_point firstFrame{};
    Clock::time_point nextNativeProbe{};
    Clock::time_point lastNativeDraw{};
    std::string screenshot;
    unsigned frames = 0;
    std::unique_ptr<cm0::NativeVideo> native;
    mpv_handle* player = nullptr;
    std::string selectedAudio;
    bool hdmiAudio = false;
    std::optional<PlaybackGain> localGain, hdmiGain;
    bool autoAudio = false, lowQuality = false, scaled = false;
    bool fullscreen = false, nativeFrame = false, drewVideo = false, drewVideoView = false;
    int targetWidth = 0, targetHeight = 0;
    bool quit = false;
    bool lcdLayout = true;
    bool startupLcdLayout = true;
    bool reopen = false;
    bool waitForVideo = false;
    const void* renderedVideo = nullptr;
    bool videoUploaded = false;
    bool saved = false;
    Clock::time_point timeout;
};
Session* session;

void selectGain(Session& s, bool hdmi) {
    if (s.hdmiAudio == hdmi) return;
    if (auto current = readGain(s.player)) (s.hdmiAudio ? s.hdmiGain : s.localGain) = current;
    s.hdmiAudio = hdmi;
    const auto& remembered = hdmi ? s.hdmiGain : s.localGain;
    if (remembered) applyGain(s.player, *remembered);
}

void configureAudio(Session& s) {
    if (!s.player || !s.autoAudio || !s.display) return;
    const char* device = "auto";
    if (std::ifstream("/proc/asound/ES8389Audio/id"))
        device = "alsa/plughw:CARD=ES8389Audio,DEV=0";
    if (s.display->usingHdmi() && cm0::hdmiAudioAvailable())
        device = "alsa/hdmi:CARD=vc4hdmi,DEV=0";
    if (s.selectedAudio != device && mpv_set_property_string(s.player, "audio-device", device) >= 0) {
        s.selectedAudio = device;
        selectGain(s, s.selectedAudio.find("hdmi") != std::string::npos);
    }
}

void configureOutput(Session& s) {
    if (!s.player) return;
    configureAudio(s);
    bool scale = s.lowQuality && !s.native;
    if (scale != s.scaled) {
        const char* args[] = {"vf", scale ? "add" : "remove", scale ?
            "@cm0-scale:lavfi=[scale=w='min(iw,640)':h='min(ih,360)':force_original_aspect_ratio=decrease:force_divisible_by=2:flags=fast_bilinear:threads=2]" :
            "@cm0-scale", nullptr};
        if (mpv_command(s.player,args)>=0) s.scaled = scale;
    }
}

void selectOutput(Session& s) {
    s.native.reset();
    s.nativeFrame = false;
    if (s.display && s.display->usingHdmi()) s.native = cm0::NativeVideo::open(640, 360);
    s.nextNativeProbe = Clock::now() + std::chrono::seconds(5);
    configureOutput(s);
}

class KeyboardHelp : public brls::Dialog {
public:
    KeyboardHelp() : brls::Dialog(cm0::compactLayout()
        ? "Fn 锁定时 5 刷新列表　H 关闭说明\n"
          "F / X / Z / C 方向　Enter 确认　Esc 返回\n"
          "Q 播放 / 暂停　W / E 换集　J 截屏\n"
          "A 静音　S / D 音量　U / I 亮度　0 退出"
        : "Fn+A 静音　S / D 音量　U / I 内屏亮度\n"
          "Q 播放 / 暂停　W / E 上一集 / 下一集\n"
          "F / X / Z / C 方向　J 截屏　H 关闭说明\n"
          "Enter 确认　Esc 返回　Fn+0 退出") {
        addButton("关闭", [] {});
    }
};

bool mediaKey(Session& s, unsigned key) {
    switch (key) {
        case KEY_HOME: case KEY_END: case KEY_PAGEUP: case KEY_PAGEDOWN: {
            size_t index = 0;
            for (auto* view = brls::Application::getCurrentFocus(); view; view = view->getParent()) {
                if (auto* item = dynamic_cast<RecyclingGridItem*>(view)) index = item->getIndex();
                auto* grid = dynamic_cast<RecyclingGrid*>(view);
                if (!grid || !grid->getItemCount()) continue;
                const size_t last = grid->getItemCount()-1;
                const size_t rows = std::max(1, int(grid->getHeight()/std::max(1.f, grid->estimatedRowHeight)));
                const size_t page = rows * std::max(1, grid->spanCount);
                if (key == KEY_HOME) index = 0;
                if (key == KEY_END) index = last;
                if (key == KEY_PAGEUP) index = index > page ? index-page : 0;
                if (key == KEY_PAGEDOWN) index = std::min(last, index+page);
                grid->selectRowAt(index, false);
                if (auto* cell = grid->getGridItemByIndex(index)) brls::Application::giveFocus(cell);
                return true;
            }
            return false;
        }
        case KEY_HELP: {
            auto stack = brls::Application::getActivitiesStack();
            if (stack.empty()) return true;
            auto* dialog = dynamic_cast<KeyboardHelp*>(stack.back()->getContentView());
            if (dialog) dialog->close(); else (new KeyboardHelp())->open();
            return true;
        }
        case KEY_SYSRQ:
        case KEY_PRINT: {
            auto pixels = s.rgb;
            if (s.native && !pixels.empty()) s.native->composeScreenshot(pixels, width, height);
            auto path = cm0::saveScreenshot(pixels, width, height);
            brls::Application::notify(path.empty() ? "截屏失败" : "已保存 " + path);
            return true;
        }
        case KEY_MUTE: case KEY_VOLUMEUP: case KEY_VOLUMEDOWN:
            if (s.hdmiAudio || (!s.player && s.display && s.display->usingHdmi())) {
                auto gain = readGain(s.player).value_or(s.hdmiGain.value_or(PlaybackGain{double(MPVCore::VIDEO_VOLUME), 0}));
                if (key == KEY_MUTE) gain.mute = !gain.mute;
                else gain.volume = std::clamp(gain.volume + (key == KEY_VOLUMEUP ? 5 : -5), 0., 100.);
                if (s.player) applyGain(s.player, gain);
                s.hdmiGain = readGain(s.player).value_or(gain);
                brls::Application::notify(s.hdmiGain->mute ? "HDMI 静音" :
                    "HDMI 音量 " + std::to_string(std::lround(s.hdmiGain->volume)) + "%");
                return true;
            }
            [[fallthrough]];
        case KEY_BRIGHTNESSUP: case KEY_BRIGHTNESSDOWN: {
            std::string feedback;
            bool brightness = key == KEY_BRIGHTNESSUP || key == KEY_BRIGHTNESSDOWN;
            bool local = (s.display && !s.display->usingHdmi()) ||
                         (!brightness && s.selectedAudio.find("ES8389Audio") != std::string::npos);
            cm0::systemHotkey(key, local, &feedback);
            if (!feedback.empty()) brls::Application::notify(feedback);
            return true;
        }
        case KEY_PLAYPAUSE: case KEY_PREVIOUSSONG: case KEY_NEXTSONG: break;
        default: return false;
    }
    if (!s.player) return true;
    auto stack = brls::Application::getActivitiesStack();
    for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
        auto* player = dynamic_cast<BasePlayerActivity*>(*it);
        if (!player && !dynamic_cast<LiveActivity*>(*it)) continue;
        if (key == KEY_PLAYPAUSE) {
            const char* args[] = {"cycle", "pause", nullptr};
            mpv_command(s.player, args);
        } else if (player && brls::Application::getCurrentFocus()) {
            bool season = dynamic_cast<PlayerSeasonActivity*>(player) != nullptr;
            if (!player->mediaListReady(season)) return true;
            if (key == KEY_NEXTSONG) player->onIndexChangeToNext();
            else if (int prev = player->previousMediaPart(season); prev >= 0) player->onIndexChange(prev);
            else if (!season) {
                auto previous = player->previousCollectionVideo();
                if (!previous.empty()) Intent::openBV(previous);
            }
        }
        break;
    }
    return true;
}

char printableKey(SDL_Scancode code) {
    const auto symbol = SDL_GetKeyFromScancode(code);
    if (symbol < 32 || symbol > 126) return 0;
    const auto mods = SDL_GetModState();
    if (mods & (KMOD_CTRL | KMOD_ALT | KMOD_GUI)) return 0;
    char value = static_cast<char>(symbol);
    if (value >= 'a' && value <= 'z') {
        if (bool(mods & KMOD_SHIFT) != bool(mods & KMOD_CAPS)) value -= 'a' - 'A';
    } else if (mods & KMOD_SHIFT) {
        constexpr std::string_view plain = "1234567890-=[]\\;',./`";
        constexpr std::string_view shifted = "!@#$%^&*()_+{}|:\"<>?~";
        auto pos = plain.find(value);
        if (pos != plain.npos) value = shifted[pos];
    }
    return value;
}

void configureCanvas(bool hdmi, bool videoControls = false) {
    width = hdmi ? (videoControls ? 640 : 1280) : 320;
    height = hdmi ? (videoControls ? 360 : 720) : 170;
}

void configureLayoutSize(bool lcd) {
    brls::Application::ORIGINAL_WINDOW_WIDTH = lcd ? 544 : 960;
    brls::Application::ORIGINAL_WINDOW_HEIGHT = lcd ? 289 : 540;
}

void createSurface(Session& s) {
    s.surface.reset();
    s.surface = cm0::LinearSurface::create(width, height);
    if (!s.surface) throw std::runtime_error("Cannot allocate the UI render target");
    glViewport(0, 0, width, height);
    std::cerr << "compositor: " << width << 'x' << height << " "
              << (s.surface->mapped() ? "linear DMA buffer" : "GLES readback") << '\n';
}

void resizeCanvas(Session& s, bool hdmi, bool controls) {
    const int previousWidth = width;
    configureCanvas(hdmi, controls);
    if (width == previousWidth) return;
    createSurface(s);
    __real_SDL_SetWindowSize(SDL_GL_GetCurrentWindow(), width, height);
    brls::Application::setWindowSize(width, height);
    std::vector<uint32_t>().swap(s.rgb);
}

void pollDevice() {
    auto& s = *session;
    auto now = Clock::now();
    if (s.display && now >= s.nextProbe) {
        const bool wasHdmi = s.display->usingHdmi();
        const auto stack = brls::Application::getActivitiesStack();
        if (s.display->update()) {
            if (wasHdmi != s.display->usingHdmi()) {
                s.lcdLayout = !s.display->usingHdmi();
                configureLayoutSize(s.lcdLayout);
                if (stack.empty()) s.startupLcdLayout = s.lcdLayout;
                else cm0::setClientDisplayLayout(s.lcdLayout);
                s.native.reset();
                s.lastNativeDraw = {};
                resizeCanvas(s, s.display->usingHdmi(), false);
                selectOutput(s);
                for (auto* activity : stack) {
                    if (auto* video = dynamic_cast<VideoView*>(activity->getView("video")))
                        cm0::refreshVideoLayout(*video, s.lcdLayout);
                }
                for (auto it = stack.rbegin(); it != stack.rend(); ++it) {
                    if (dynamic_cast<LiveActivity*>(*it)) break;
                    if (auto* player = dynamic_cast<BasePlayerActivity*>(*it)) {
                        player->refreshOutputQuality(dynamic_cast<PlayerSeasonActivity*>(player) != nullptr);
                        break;
                    }
                }
            } else {
                selectOutput(s);
                if (!s.rgb.empty()) s.display->present(s.rgb, width, height);
            }
        }
        // The player can update its existing controls. Browsing layouts can
        // have a different view tree, so rebuild once playback has been left.
        if (s.startupLcdLayout != s.lcdLayout && !stack.empty()) {
            const bool playing = std::any_of(stack.begin(), stack.end(), [](auto* activity) {
                return dynamic_cast<BasePlayerActivity*>(activity) ||
                    dynamic_cast<LiveActivity*>(activity) ||
                    dynamic_cast<VideoView*>(activity->getView("video"));
            });
            if (!playing) {
                s.reopen = s.quit = true;
            }
        }
        if (!s.native && s.display->usingHdmi() && now >= s.nextNativeProbe) selectOutput(s);
        // The monitor can publish its audio capabilities after the video connection.
        configureAudio(s);
        s.nextProbe = now + std::chrono::milliseconds(250);
    }
    if (s.display && s.display->usingHdmi() && !s.quit) {
        // Avoid resizing for a single missing video frame, e.g. during a seek.
        const bool controls = s.native && s.lastNativeDraw != Clock::time_point{} &&
            now - s.lastNativeDraw < std::chrono::seconds(1);
        resizeCanvas(s, true, controls);
    }
    if (!s.keyboard) return;
    for (auto key : s.keyboard->poll()) {
        if (key.down && s.lcdLayout) {
            const auto stack = brls::Application::getActivitiesStack();
            if (!stack.empty()) {
                if (key.code == KEY_ESC && dynamic_cast<SettingActivity*>(stack.back())) {
                    brls::Application::popActivity(brls::TransitionAnimation::NONE);
                    continue;
                }
                if (auto* login = dynamic_cast<MineQrLogin*>(stack.back()->getContentView())) {
                    login->closeLcdLogin();
                    continue;
                }
            }
        }
        if (key.down && mediaKey(s, key.code)) continue;
        if (key.code == KEY_F10 && key.down) s.quit = true;
        if (key.code >= std::size(linux_scancode_table)) continue;
        const auto code = linux_scancode_table[key.code];
        if (code == SDL_SCANCODE_UNKNOWN) continue;
        const char value = printableKey(code);
        if (key.down && s.lcdLayout && (value || code == SDL_SCANCODE_BACKSPACE)) {
            const auto stack = brls::Application::getActivitiesStack();
            auto* search = stack.empty() ? nullptr : dynamic_cast<cm0::LcdSearchActivity*>(stack.back());
            if (search) {
                if (value) search->type(value);
                else search->backspace();
                continue;
            }
        }
        SDL_SendKeyboardKey(key.down ? SDL_PRESSED : SDL_RELEASED, code);
        if (!key.down || !SDL_IsTextInputActive() || !value) continue;
        char text[] = {value, 0};
        SDL_SendKeyboardText(text);
    }
}

void saveFrame(const Session& s) {
    std::ofstream file(s.screenshot, std::ios::binary);
    file << "P6\n" << width << ' ' << height << "\n255\n";
    for (auto pixel : s.rgb) {
        char rgb[] = {char(pixel >> 16), char(pixel >> 8), char(pixel)};
        file.write(rgb, 3);
    }
    if (!file) throw std::runtime_error("Cannot write " + s.screenshot);
}
}

void cm0::setSearchTyping(bool active) {
    if (session && session->keyboard) cm0::setKeypadFnLock(!active);
}

bool cm0::compactLayout() { return session && session->lcdLayout; }

bool cm0::usingLcd() {
    return session && session->display && !session->display->usingHdmi();
}

void cm0::attachPlayer(mpv_handle* handle, bool lowQuality) {
    if (!session) return;
    auto& s = *session;
    if (auto last = readGain(s.player)) (s.hdmiAudio ? s.hdmiGain : s.localGain) = last;
    s.player = handle;
    s.hdmiAudio = false;
    s.selectedAudio.clear();
    s.lowQuality = lowQuality;
    s.scaled = s.nativeFrame = false;
    if (!handle) {
        if (s.native) s.native->hide();
        return;
    }
    char* device = mpv_get_property_string(handle,"audio-device");
    s.autoAudio = device && std::string_view(device) == "auto";
    if (s.localGain) applyGain(handle, *s.localGain);
    if (!s.autoAudio && device) {
        s.selectedAudio = device;
        selectGain(s, s.selectedAudio.find("hdmi") != std::string::npos);
    }
    mpv_free(device);
    configureOutput(s);
}

void cm0::setVideoFullscreen(bool fullscreen) {
    if (session) session->fullscreen = fullscreen;
}

bool cm0::drawVideoPlane(NVGcontext* vg, float x, float y, float w, float h, float alpha) {
    auto& s = *session;
    s.drewVideoView = true;
    if (!s.native || !s.nativeFrame || !s.fullscreen || alpha < 0.999f) return false;
    const auto& v = s.native->viewport;
    nvgSave(vg);
    nvgBeginPath(vg);
    nvgFillColor(vg,nvgRGBA(0,0,0,255));
    nvgRect(vg,x,y,w,h);
    nvgFill(vg);
    nvgGlobalCompositeOperation(vg,NVG_COPY);
    nvgBeginPath(vg);
    nvgRect(vg,x+w*v[0],y+h*v[1],w*(v[2]-v[0]),h*(v[3]-v[1]));
    nvgFillColor(vg,nvgRGBA(0,0,0,0));
    nvgFill(vg);
    nvgRestore(vg);
    s.drewVideo = true;
    return true;
}

cm0::RollingDanmaku cm0::rollingDanmaku(NVGcontext* vg, float x, float y, float w, float h,
                                     float lineHeight, bool smartMask) {
    RollingDanmaku result;
    if (!session || lineHeight <= 0) return result;
    float transform[6];
    nvgCurrentTransform(vg, transform);
    if (transform[0] <= 0 || transform[0] != transform[3] || transform[1] || transform[2])
        return result;
    if (usingLcd()) {
        result.maxLines = 2;
        result.fontScale = 12.0f / (30 * transform[0]);
        return result;
    }
    auto& s = *session;
    if (smartMask || width != 640 || !s.drewVideo) return result;
    result.scale = transform[0] * 2;
    result.offsetX = transform[4] * 2;
    result.offsetY = transform[5] * 2;
    const auto px = [&](float value, float offset) { return int(std::lround(value * result.scale + offset)); };
    result.clip = {px(x, result.offsetX), px(y, result.offsetY), px(x+w, result.offsetX), px(y+h, result.offsetY)};
    const int limit = std::min(result.clip[3], DanmakuStrip::height);
    const float space = limit - px(y+5, result.offsetY);
    if (space < 40) return result;
    result.strip = s.native->requestDanmaku();
    if (result.strip) {
        result.maxLines = std::max(1, 1 + int((space - 40) / (lineHeight * result.scale)));
        result.fontScale = 27.0f / (30 * result.scale);
    }
    return result;
}

bool cm0::RollingDanmaku::paint(std::string_view text, float size, uint32_t color,
                               float alpha, float x, float y) const {
    return strip && strip->paint(text, std::lround(size * scale), color,
        std::lround(255 * std::clamp(alpha, 0.0f, 1.0f)),
        std::lround(x * scale + offsetX), std::lround(y * scale + offsetY), clip);
}

extern "C" SDL_Window* __wrap_SDL_CreateWindow(const char* title, int x, int y, int, int, Uint32 flags) {
    configureLayoutSize(session->lcdLayout);
    brls::Application::setLimitedFPS(30);
    auto window = __real_SDL_CreateWindow(title, x, y, width, height,
                                          flags & ~(SDL_WINDOW_FULLSCREEN_DESKTOP | SDL_WINDOW_RESIZABLE));
    if (window) SDL_SetKeyboardFocus(window);
    return window;
}

// Only the output selector changes the drawable size. Ignore desktop window preferences.
extern "C" void __wrap_SDL_SetWindowSize(SDL_Window*, int, int) {}
extern "C" void __wrap_SDL_SetWindowMinimumSize(SDL_Window*, int, int) {}
extern "C" void __wrap_SDL_SetWindowMaximumSize(SDL_Window*, int, int) {}
extern "C" int __wrap_SDL_SetWindowFullscreen(SDL_Window*, Uint32) { return 0; }

// All UI pixels live in the resizable FBO. SDL only needs a current EGL surface.
extern "C" EGLSurface __wrap_SDL_EGL_CreateOffscreenSurface(SDL_VideoDevice* device, int, int) {
    return __real_SDL_EGL_CreateOffscreenSurface(device, 1, 1);
}

extern "C" SDL_GLContext __wrap_SDL_GL_CreateContext(SDL_Window* window) {
    auto context = __real_SDL_GL_CreateContext(window);
    if (!context) throw std::runtime_error(std::string("Cannot create GLES context: ") + SDL_GetError());
    createSurface(*session);
    return context;
}

extern "C" int __wrap_SDL_PollEvent(SDL_Event* event) {
    pollDevice();
    if (!session->screenshot.empty() && Clock::now() >= session->timeout) session->quit = true;
    if ((session->quit || cm0::stopping()) && event) {
        *event = {};
        event->type = SDL_QUIT;
        return 1;
    }
    return __real_SDL_PollEvent(event);
}

// Follow a fresh mpv frame through its software buffer into the NanoVG texture.
extern "C" int __wrap_mpv_render_context_render(mpv_render_context* ctx, mpv_render_param* params) {
    if (session && session->native && session->fullscreen) {
        auto& s = *session;
        s.nativeFrame = false;
        mpv_cm0_present sink{[](const mpv_cm0_frame* f) {
            auto& s = *session;
            s.nativeFrame = s.native->upload(*f,s.targetWidth,s.targetHeight);
            return int(s.nativeFrame);
        }};
        std::array<mpv_render_param,16> args{};
        unsigned count = 0;
        for (auto* p=params; p->type!=MPV_RENDER_PARAM_INVALID; ++p) {
            if (count+2 >= args.size()) return __real_mpv_render_context_render(ctx,params);
            args[count++] = *p;
            if (p->type==MPV_RENDER_PARAM_SW_SIZE) {
                auto* size=static_cast<const int*>(p->data);
                s.targetWidth=size[0]; s.targetHeight=size[1];
            }
        }
        args[count] = {MPV_CM0_FRAME_PARAM,&sink};
        return __real_mpv_render_context_render(ctx,args.data());
    }
    if (session) session->nativeFrame = false;
    if (!session || !session->waitForVideo)
        return __real_mpv_render_context_render(ctx, params);
    mpv_render_frame_info info{};
    mpv_render_context_get_info(ctx, {MPV_RENDER_PARAM_NEXT_FRAME_INFO, &info});
    const int status = __real_mpv_render_context_render(ctx, params);
    session->renderedVideo = nullptr;
    if (status >= 0 && (info.flags & MPV_RENDER_FRAME_INFO_PRESENT) &&
        !(info.flags & (MPV_RENDER_FRAME_INFO_REDRAW | MPV_RENDER_FRAME_INFO_REPEAT))) {
        for (auto* p = params; p->type != MPV_RENDER_PARAM_INVALID; ++p)
            if (p->type == MPV_RENDER_PARAM_SW_POINTER) session->renderedVideo = p->data;
    }
    return status;
}

extern "C" void __wrap_nvgUpdateImage(NVGcontext* vg, int id, const unsigned char* pixels) {
    __real_nvgUpdateImage(vg, id, pixels);
    if (session && pixels && pixels == session->renderedVideo && id > 0)
        session->videoUploaded = true;
}

extern "C" void __wrap_SDL_GL_SwapWindow(SDL_Window* window) {
    auto& s = *session;
    const bool fullscreen = std::exchange(s.drewVideoView, false) && s.fullscreen;
    if (s.drewVideo) s.lastNativeDraw = Clock::now();
    const bool leavingCompactCanvas = width == 640 && !fullscreen && !s.drewVideo;
    if (leavingCompactCanvas) {
        s.lastNativeDraw = {};
        std::cerr << "HDMI fullscreen exit: waiting for the 1280x720 canvas\n";
    }
    if ((s.drewVideo && width > 640) || leavingCompactCanvas) {
        // Hold the last scanout until the next frame draws into the new canvas.
        s.drewVideo = s.videoUploaded = false;
        __real_SDL_GL_SwapWindow(window);
        return;
    }
    s.rgb.resize(width * height);
    if (!s.surface->read(s.rgb)) {
        // GLES returns bottom-up RGBA. Convert the same allocation to top-down ARGB.
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, s.rgb.data());
        for (auto& pixel : s.rgb) {
            const auto* rgba = reinterpret_cast<const unsigned char*>(&pixel);
            pixel = (uint32_t(rgba[3]) << 24) | (uint32_t(rgba[0]) << 16) | (uint32_t(rgba[1]) << 8) | rgba[2];
        }
        for (int y = 0; y < height / 2; ++y) {
            auto* row = s.rgb.data() + y * width;
            std::swap_ranges(row, row + width, s.rgb.data() + (height - 1 - y) * width);
        }
    }
    if (s.frames++ == 0) s.firstFrame = Clock::now();
    bool presented = false;
    if (s.native) {
        if (s.drewVideo) {
            presented = s.native->show(s.rgb,width,height);
            if (!presented) {
                s.native.reset();
                s.nextNativeProbe = Clock::now() + std::chrono::seconds(5);
                s.nativeFrame = false;
                configureOutput(s);
            }
        } else s.native->hide();
    }
    s.drewVideo = false;
    if (s.display && !presented) s.display->present(s.rgb, width, height);
    if (!s.screenshot.empty() && s.frames >= 3 && Clock::now() - s.firstFrame >= std::chrono::seconds(2) &&
        (!s.waitForVideo || (s.videoUploaded && MPVCore::instance().playback_time >= 1.0))) {
        saveFrame(s);
        s.saved = true;
        s.quit = true;
    }
    s.videoUploaded = false;
    __real_SDL_GL_SwapWindow(window);
}

int main(int argc, char** argv) {
    std::string display = "auto", screenshot, bvid;
    std::vector<char*> args{argv[0]};
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--help") {
            std::cout << "wiliwili-cpz for CardputerZero\n"
                         "--display auto|lcd|hdmi   --bv BV_ID   --smoke-test FILE.ppm\n"
                         "Arrows: navigate   Enter: select   Esc: back   F10: quit\n";
            return 0;
        }
        if (arg == "--display" || arg == "--smoke-test") {
            if (++i == argc) return 2;
            (arg == "--display" ? display : screenshot) = argv[i];
        } else if (arg == "--bv") {
            if (++i == argc) return 2;
            bvid = argv[i];
            if (bvid.size() != 12 || bvid.compare(0, 2, "BV") ||
                !std::all_of(bvid.begin() + 2, bvid.end(), [](unsigned char c) { return std::isalnum(c); }))
                return 2;
        } else args.push_back(argv[i]);
    }
    if (display != "auto" && display != "lcd" && display != "hdmi") return 2;
    args.push_back(nullptr);
    auto run = [&] {
        // Keep freed decoder and demux buffers reusable across worker threads on this board.
        mallopt(M_ARENA_MAX, 2);
        mallopt(M_MMAP_THRESHOLD, 128 * 1024);
        Session state;
        state.screenshot = screenshot;
        state.waitForVideo = !screenshot.empty() && !bvid.empty();
        state.timeout = Clock::now() + std::chrono::seconds(90);
        if (screenshot.empty()) {
            state.display = std::make_unique<cm0::Display>(display);
            state.keyboard = std::make_unique<cm0::Keyboard>();
        }
        state.lcdLayout = state.display ? !state.display->usingHdmi() : display != "hdmi";
        state.startupLcdLayout = state.lcdLayout;
        configureCanvas(!state.lcdLayout);
        session = &state;
        selectOutput(state);
        umask(0077); // Settings can contain login cookies.
        setenv("SDL_VIDEODRIVER", "offscreen", 1);
        setenv("LIBGL_DRIVERS_PATH", "/usr/lib/wiliwili-cpz/dri", 0);
        setenv("GBM_BACKENDS_PATH", "/usr/lib/wiliwili-cpz/gbm", 0);
        const int result = runClient(static_cast<int>(args.size() - 1), args.data(), bvid);
        session = nullptr;
        if (!screenshot.empty() && !state.saved) return 1;
        return state.reopen ? 75 : result;
    };
    try {
        if (!screenshot.empty()) return run();
        return cm0::supervise(run);
    } catch (const std::exception& error) {
        std::cerr << "wiliwili-cpz: " << error.what() << '\n';
        return 1;
    }
}
