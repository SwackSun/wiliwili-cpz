#include "device-runtime.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

#include <fcntl.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/kd.h>
#include <linux/vt.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <termios.h>
#include <unistd.h>

namespace cm0 {
namespace {
namespace fs = std::filesystem;
using Clock = std::chrono::steady_clock;

class File {
public:
    explicit File(int fd) : fd_(fd) {}
    ~File() {
        if (fd_ >= 0) close(fd_);
    }
    File(const File&) = delete;
    File& operator=(const File&) = delete;
    int get() const { return fd_; }
    explicit operator bool() const { return fd_ >= 0; }

private:
    int fd_;
};

std::string firstLine(const fs::path& path) {
    std::ifstream file(path);
    std::string value;
    std::getline(file, value);
    return value;
}

class Backlight {
public:
    Backlight() {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator("/sys/class/backlight", error)) {
            auto path = entry.path() / "bl_power";
            if (std::ifstream(path) >> savedPower_) {
                path_ = std::move(path);
                break;
            }
        }
    }

    ~Backlight() { restore(); }

    void setForOutput(bool hdmi) const {
        if (!path_.empty() && !(std::ofstream(path_) << (hdmi ? 4 : 0) << '\n'))
            throw std::runtime_error("Cannot set LCD backlight power");
    }

    void restore() const {
        if (!path_.empty()) std::ofstream(path_) << savedPower_ << '\n';
    }

private:
    fs::path path_;
    int savedPower_ = 0;
};

class Console {
public:
    Console()
        : lock_(lockDisplay()), tty_(open("/dev/tty1", O_RDWR | O_NOCTTY | O_CLOEXEC)) {
        vt_stat state{};
        vt_mode mode{};
        int displayMode = -1;
        if (!tty_ || ioctl(tty_.get(), VT_GETSTATE, &state) || ioctl(tty_.get(), VT_GETMODE, &mode) ||
            ioctl(tty_.get(), KDGETMODE, &displayMode) || ioctl(tty_.get(), KDGKBMODE, &keyboardMode_) ||
            state.v_active != 1 || mode.mode != VT_AUTO || displayMode != KD_TEXT)
            throw std::runtime_error("tty1 must be active in text mode before starting wiliwili-cpz");
        if (ioctl(tty_.get(), KDSETMODE, KD_GRAPHICS))
            throw std::runtime_error("Cannot enter graphics mode");
        active_ = true;
        if (ioctl(tty_.get(), KDSKBMODE, K_OFF)) {
            restore();
            throw std::runtime_error("Cannot disable console keyboard forwarding");
        }
        std::cerr << "console: graphics acquired, supervisor=" << getpid() << '\n';
    }

    ~Console() { restore(); }

    void restore() {
        if (!active_) return;
        active_ = false;
        setKeypadFnLock(false);
        tcflush(tty_.get(), TCIFLUSH);
        if (ioctl(tty_.get(), KDSKBMODE, keyboardMode_)) perror("restore keyboard");
        if (ioctl(tty_.get(), KDSETMODE, KD_TEXT)) perror("restore text mode");
    }

private:
    static int lockDisplay() {
        fs::create_directories("/run/cm0");
        // All foreground apps must agree on the display lease used by czlite.
        int lock = open("/run/cm0/game.lock", O_RDWR | O_CREAT | O_CLOEXEC, 0600);
        if (lock >= 0 && flock(lock, LOCK_EX | LOCK_NB) == 0) return lock;
        if (lock >= 0) close(lock);
        throw std::runtime_error("Cannot acquire the foreground display");
    }

    File lock_;
    File tty_;
    int keyboardMode_ = -1;
    bool active_ = false;
};

volatile sig_atomic_t stopRequested = 0;
void requestStop(int) { stopRequested = 1; }

void removeClientPid() {
    std::error_code error;
    fs::remove("/run/cm0/game.pid", error);
}

bool hdmiConnected() {
    std::error_code error;
    for (const auto& entry : fs::directory_iterator("/sys/class/drm", error)) {
        const auto name = entry.path().filename().string();
        if (name.find("-HDMI-A-") != std::string::npos && firstLine(entry.path() / "status") == "connected")
            return true;
    }
    return false;
}

std::string framebufferPath(bool hdmi) {
    for (const auto& entry : fs::directory_iterator("/sys/class/graphics")) {
        const auto name = entry.path().filename().string();
        if (name.compare(0, 2, "fb") || name.size() < 3 || name.find_first_not_of("0123456789", 2) != name.npos)
            continue;
        const auto driver = firstLine(entry.path() / "name");
        const bool match = hdmi ? driver == "vc4drmfb" :
            driver.find("panel-mipi") != driver.npos || driver.find("st7789") != driver.npos;
        if (match) return "/dev/" + name;
    }
    throw std::runtime_error(hdmi ? "HDMI framebuffer unavailable" : "LCD framebuffer unavailable");
}

std::array<fb_bitfield, 4> channels(const fb_var_screeninfo& mode) {
    return {mode.red, mode.green, mode.blue, mode.transp};
}

bool sameChannels(const fb_var_screeninfo& a, const fb_var_screeninfo& b) {
    const auto left = channels(a);
    const auto right = channels(b);
    return std::equal(left.begin(), left.end(), right.begin(), [](auto x, auto y) {
        return x.offset == y.offset && x.length == y.length && x.msb_right == y.msb_right;
    });
}

class Framebuffer {
public:
    explicit Framebuffer(std::string path) : path_(std::move(path)), fd_(open(path_.c_str(), O_RDWR | O_CLOEXEC)) {
        if (!fd_ || !readMode(mode_, fixed_)) throw std::runtime_error("Cannot query " + path_);
        validate();
        preparePalette();
        mapping_ = mmap(nullptr, fixed_.smem_len, PROT_READ | PROT_WRITE, MAP_SHARED, fd_.get(), 0);
        if (mapping_ == MAP_FAILED) throw std::runtime_error("Cannot map " + path_);
    }

    ~Framebuffer() { munmap(mapping_, fixed_.smem_len); }

    bool current(const std::string& path) const {
        fb_var_screeninfo mode{};
        fb_fix_screeninfo fixed{};
        return path == path_ && readMode(mode, fixed) && mode.xres == mode_.xres && mode.yres == mode_.yres &&
            mode.xoffset == mode_.xoffset && mode.yoffset == mode_.yoffset && mode.bits_per_pixel == mode_.bits_per_pixel &&
            mode.grayscale == mode_.grayscale && mode.nonstd == mode_.nonstd && sameChannels(mode, mode_) &&
            fixed.type == fixed_.type && fixed.visual == fixed_.visual && fixed.smem_start == fixed_.smem_start &&
            fixed.smem_len == fixed_.smem_len && fixed.line_length == fixed_.line_length;
    }

    void draw(const std::vector<std::uint32_t>& pixels, int width, int height) {
        if (width < 1 || width > 4096 || height < 1 || height > 2160 ||
            pixels.size() != static_cast<std::size_t>(width) * height)
            throw std::runtime_error("Invalid wiliwili-cpz frame size");
        if (width != sourceWidth_ || height != sourceHeight_) configureViewport(width, height);
        if (mode_.bits_per_pixel == 16) drawRows(pixels, row16_);
        else drawRows(pixels, row32_);
        previous_ = pixels;
    }

    std::string description() const {
        return path_ + " " + std::to_string(mode_.xres) + "x" + std::to_string(mode_.yres);
    }

private:
    bool readMode(fb_var_screeninfo& mode, fb_fix_screeninfo& fixed) const {
        return ioctl(fd_.get(), FBIOGET_VSCREENINFO, &mode) == 0 && ioctl(fd_.get(), FBIOGET_FSCREENINFO, &fixed) == 0;
    }

    void validate() const {
        if (!mode_.xres || mode_.xres > 4096 || !mode_.yres || mode_.yres > 2160 ||
            (mode_.bits_per_pixel != 16 && mode_.bits_per_pixel != 32) ||
            fixed_.type != FB_TYPE_PACKED_PIXELS || fixed_.visual != FB_VISUAL_TRUECOLOR ||
            mode_.grayscale || mode_.nonstd || !mode_.red.length || !mode_.green.length || !mode_.blue.length)
            throw std::runtime_error("Unsupported framebuffer mode on " + path_);
        const std::uint64_t right = (std::uint64_t(mode_.xoffset) + mode_.xres) * bytesPerPixel();
        const std::uint64_t lastRow = std::uint64_t(mode_.yoffset) + mode_.yres - 1;
        if (right > fixed_.line_length || right > fixed_.smem_len ||
            lastRow > (fixed_.smem_len - right) / fixed_.line_length)
            throw std::runtime_error("Framebuffer viewport exceeds the mapping");
    }

    void preparePalette() {
        const auto fields = channels(mode_);
        std::uint32_t used = 0;
        for (std::size_t i = 0; i < fields.size(); ++i) {
            const auto field = fields[i];
            if (field.length > 8 || field.offset > mode_.bits_per_pixel ||
                field.length > mode_.bits_per_pixel - field.offset || field.msb_right)
                throw std::runtime_error("Unsupported framebuffer color channel");
            if (!field.length) continue;
            const auto mask = ((1u << field.length) - 1u) << field.offset;
            if (used & mask) throw std::runtime_error("Overlapping framebuffer color channels");
            used |= mask;
            if (i == 3) alpha_ = mask;
            else for (unsigned value = 0; value < 256; ++value)
                palette_[i][value] = (value >> (8 - field.length)) << field.offset;
        }
    }

    unsigned bytesPerPixel() const { return mode_.bits_per_pixel / 8; }

    unsigned char* rowAddress(unsigned y) const {
        return static_cast<unsigned char*>(mapping_) +
            (static_cast<std::size_t>(y) + mode_.yoffset) * fixed_.line_length +
            static_cast<std::size_t>(mode_.xoffset) * bytesPerPixel();
    }

    void configureViewport(int width, int height) {
        sourceWidth_ = width;
        sourceHeight_ = height;
        const int scaledWidth = std::max(1, std::min(int(mode_.xres), int(mode_.yres) * width / height));
        const int scaledHeight = std::max(1, std::min(int(mode_.yres), scaledWidth * height / width));
        left_ = (mode_.xres - scaledWidth) / 2;
        top_ = (mode_.yres - scaledHeight) / 2;
        columns_.resize(scaledWidth);
        rows_.resize(scaledHeight);
        row16_.resize(mode_.bits_per_pixel == 16 ? scaledWidth : 0);
        row32_.resize(mode_.bits_per_pixel == 32 ? scaledWidth : 0);
        for (int x = 0; x < scaledWidth; ++x) columns_[x] = x * width / scaledWidth;
        for (int y = 0; y < scaledHeight; ++y) rows_[y] = y * height / scaledHeight;
        for (unsigned y = 0; y < mode_.yres; ++y)
            std::memset(rowAddress(y), 0, mode_.xres * bytesPerPixel());
        std::vector<std::uint32_t>().swap(previous_);
    }

    template<class Pixel>
    void drawRows(const std::vector<std::uint32_t>& pixels, std::vector<Pixel>& output) {
        for (std::size_t y = 0; y < rows_.size();) {
            const int sourceRow = rows_[y];
            const auto next = static_cast<std::size_t>(
                std::upper_bound(rows_.begin() + y, rows_.end(), sourceRow) - rows_.begin());
            const auto* source = pixels.data() + sourceRow * sourceWidth_;
            int start = 0;
            int end = sourceWidth_;
            if (!previous_.empty()) {
                const auto* old = previous_.data() + sourceRow * sourceWidth_;
                start = std::mismatch(source, source + sourceWidth_, old).first - source;
                while (end > start && source[end - 1] == old[end - 1]) --end;
            }
            const auto first = std::lower_bound(columns_.begin(), columns_.end(), start) - columns_.begin();
            const auto last = std::lower_bound(columns_.begin(), columns_.end(), end) - columns_.begin();
            if (first != last) {
                for (auto x = first; x < last; ++x) {
                    const auto rgb = source[columns_[x]];
                    output[x] = palette_[0][(rgb >> 16) & 255] | palette_[1][(rgb >> 8) & 255] |
                        palette_[2][rgb & 255] | alpha_;
                }
                for (auto dest = y; dest < next; ++dest)
                    std::memcpy(rowAddress(top_ + dest) + (left_ + first) * sizeof(Pixel),
                                output.data() + first, (last - first) * sizeof(Pixel));
            }
            y = next;
        }
    }

    std::string path_;
    File fd_;
    fb_var_screeninfo mode_{};
    fb_fix_screeninfo fixed_{};
    void* mapping_ = MAP_FAILED;
    std::array<std::array<std::uint32_t, 256>, 3> palette_{};
    std::uint32_t alpha_ = 0;
    int sourceWidth_ = 0, sourceHeight_ = 0;
    unsigned top_ = 0, left_ = 0;
    std::vector<int> columns_, rows_;
    std::vector<std::uint32_t> previous_, row32_;
    std::vector<std::uint16_t> row16_;
};

using KeyBits = std::array<unsigned char, (KEY_CNT + 7) / 8>;
bool bit(const KeyBits& keys, unsigned code) {
    return (keys[code / 8] & (1u << (code % 8))) != 0;
}
}

void setKeypadFnLock(bool lock) {
    static const fs::path attribute = [] {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator("/sys/bus/i2c/drivers/tca8418_keypad", error)) {
            const auto path = entry.path() / "fn_lock";
            if (fs::exists(path, error)) return path;
        }
        return fs::path{};
    }();
    if (!attribute.empty()) std::ofstream(attribute) << (lock ? "1\n" : "0\n");
}

struct Display::State {
    explicit State(std::string selected) : choice(std::move(selected)) {}

    bool select(bool hdmi) {
        const auto path = framebufferPath(hdmi);
        if (framebuffer && usingHdmi == hdmi && framebuffer->current(path)) return false;
        auto output = std::make_unique<Framebuffer>(path);
        backlight.setForOutput(hdmi);
        framebuffer = std::move(output);
        usingHdmi = hdmi;
        std::cerr << "display: " << (hdmi ? "HDMI " : "LCD ") << framebuffer->description() << '\n';
        return true;
    }

    std::string choice;
    Backlight backlight;
    std::unique_ptr<Framebuffer> framebuffer;
    bool usingHdmi = false;
};

Display::Display(std::string choice) : state_(std::make_unique<State>(std::move(choice))) { update(); }
Display::~Display() = default;

bool Display::update() {
    const bool connected = state_->choice != "lcd" && hdmiConnected();
    if (state_->choice == "hdmi" && !connected && !state_->framebuffer)
        throw std::runtime_error("HDMI is not connected");
    if (connected) {
        try { return state_->select(true); }
        catch (const std::exception& error) {
            if (state_->framebuffer && !state_->usingHdmi) return false;
            std::cerr << "HDMI framebuffer unavailable: " << error.what() << '\n';
        }
    }
    return state_->select(false);
}

void Display::present(const std::vector<std::uint32_t>& rgb, int width, int height) {
    state_->framebuffer->draw(rgb, width, height);
}

bool Display::usingHdmi() const { return state_->usingHdmi; }

struct Keyboard::State {
    struct Device {
        explicit Device(fs::path devicePath)
            : path(std::move(devicePath)), fd(open(path.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC)) {}
        fs::path path;
        File fd;
        std::array<bool, KEY_CNT> down{};
        bool syncing = false;
    };

    void scan() {
        std::error_code error;
        for (const auto& entry : fs::directory_iterator("/dev/input", error)) {
            const auto path = entry.path();
            if (path.filename().string().compare(0, 5, "event")) continue;
            if (std::any_of(devices.begin(), devices.end(), [&](const auto& device) { return device->path == path; }))
                continue;
            struct stat info{};
            if (stat(path.c_str(), &info) != 0) continue;
            const auto identity = std::make_pair(info.st_rdev, info.st_ino);
            const auto known = ignored.find(path);
            if (known != ignored.end() && known->second == identity) continue;
            auto device = std::make_unique<Device>(path);
            if (!device->fd) continue;
            KeyBits keys{};
            if (ioctl(device->fd.get(), EVIOCGBIT(EV_KEY, keys.size()), keys.data()) < 0) continue;
            if (!bit(keys, KEY_A) || !bit(keys, KEY_Z) || !bit(keys, KEY_ENTER)) {
                // Keep HDMI CEC and other non-keyboard devices closed until their nodes change.
                ignored[path] = identity;
                continue;
            }
            if (ioctl(device->fd.get(), EVIOCGRAB, 1) == 0) {
                std::cerr << "keyboard: " << device->path << '\n';
                devices.push_back(std::move(device));
            }
        }
        nextScan = Clock::now() + std::chrono::seconds(1);
    }

    static void send(Device& device, unsigned code, bool down, std::vector<KeyEvent>& events) {
        if (device.down[code] == down) return;
        device.down[code] = down;
        events.push_back({code, down});
    }

    static bool drain(Device& device, std::vector<KeyEvent>& events) {
        input_event input{};
        for (;;) {
            const auto received = read(device.fd.get(), &input, sizeof(input));
            if (received < 0 && errno == EINTR) continue;
            if (received < 0 && errno == EAGAIN) return true;
            if (received != sizeof(input)) return false;
            if (input.type == EV_SYN && input.code == SYN_DROPPED) {
                device.syncing = true;
                continue;
            }
            if (device.syncing) {
                if (input.type != EV_SYN || input.code != SYN_REPORT) continue;
                KeyBits keys{};
                if (ioctl(device.fd.get(), EVIOCGKEY(keys.size()), keys.data()) < 0) keys.fill(0);
                for (unsigned code = 0; code < KEY_CNT; ++code)
                    if (!bit(keys, code)) send(device, code, false, events);
                device.syncing = false;
            } else if (input.type == EV_KEY && input.code < KEY_CNT && (input.value == 0 || input.value == 1)) {
                send(device, input.code, input.value == 1, events);
            }
        }
    }

    std::vector<std::unique_ptr<Device>> devices;
    std::map<fs::path, std::pair<dev_t, ino_t>> ignored;
    Clock::time_point nextScan{};
};

Keyboard::Keyboard() : state_(std::make_unique<State>()) {
    state_->scan();
    if (state_->devices.empty()) throw std::runtime_error("No keyboard available");
}

Keyboard::~Keyboard() = default;

std::vector<KeyEvent> Keyboard::poll() {
    std::vector<KeyEvent> events;
    auto& devices = state_->devices;
    for (auto it = devices.begin(); it != devices.end();) {
        if (State::drain(**it, events)) {
            ++it;
        } else {
            for (unsigned code = 0; code < KEY_CNT; ++code) State::send(**it, code, false, events);
            it = devices.erase(it);
        }
    }
    if (Clock::now() >= state_->nextScan) state_->scan();
    return events;
}

static int runChild(const std::function<int()>& client, Console& console, Backlight& backlight) {
    const auto supervisor = getpid();
    const auto pid = fork();
    if (pid < 0) throw std::runtime_error("Cannot start wiliwili-cpz");
    if (pid == 0) {
        int status = 1;
        if (prctl(PR_SET_PDEATHSIG, SIGTERM) == 0 && getppid() == supervisor) {
            try { status = client(); }
            catch (const std::exception& error) { std::cerr << "wiliwili-cpz: " << error.what() << '\n'; }
        }
        if (getppid() != supervisor) {
            backlight.restore();
            removeClientPid();
            console.restore();
        }
        std::cerr.flush();
        std::cout.flush();
        _exit(status);
    }

    std::ofstream("/run/cm0/game.pid") << pid << '\n';
    int status = 0;
    auto deadline = Clock::time_point::max();
    for (;;) {
        const auto completed = waitpid(pid, &status, WNOHANG);
        if (completed == pid) break;
        if (completed < 0 && errno != EINTR) {
            status = 1 << 8;
            break;
        }
        if (stopping()) {
            const auto now = Clock::now();
            if (deadline == Clock::time_point::max()) {
                kill(pid, SIGTERM);
                deadline = now + std::chrono::seconds(3);
            } else if (now >= deadline) {
                kill(pid, SIGKILL);
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    removeClientPid();
    return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
}

int supervise(const std::function<int()>& client) {
    stopRequested = 0;
    Console console;
    Backlight backlight;
    for (int signal : {SIGINT, SIGTERM, SIGHUP}) std::signal(signal, requestStop);
    setKeypadFnLock(true);
    int status;
    do {
        status = runChild(client, console, backlight);
    } while (status == 75 && !stopping());
    backlight.restore();
    console.restore();
    std::cerr << "console: text restored, child status=" << status << '\n';
    return status;
}

bool stopping() { return stopRequested != 0; }

bool hdmiAudioAvailable() {
    std::ifstream info("/proc/asound/vc4hdmi/eld#0");
    std::string field, value;
    while (info >> field && std::getline(info, value)) {
        if (field == "sad_count") return std::strtol(value.c_str(), nullptr, 10) > 0;
    }
    return false;
}

bool systemHotkey(unsigned code, bool localDisplay, std::string* feedback) {
    const char* command;
    switch (code) {
        case KEY_BRIGHTNESSUP: command = "/usr/bin/cm0-hotkey brighter"; break;
        case KEY_BRIGHTNESSDOWN: command = "/usr/bin/cm0-hotkey dimmer"; break;
        case KEY_VOLUMEUP: command = "/usr/bin/cm0-hotkey volume-up"; break;
        case KEY_VOLUMEDOWN: command = "/usr/bin/cm0-hotkey volume-down"; break;
        case KEY_MUTE: command = "/usr/bin/cm0-hotkey mute"; break;
        default: return false;
    }
    if (localDisplay) {
        if (FILE* pipe = popen(command, "r")) {
            char buffer[128]{};
            const bool readFeedback = fgets(buffer, sizeof(buffer), pipe) != nullptr;
            if (pclose(pipe) == 0 && readFeedback && feedback)
                feedback->assign(buffer, std::strcspn(buffer, "\r\n"));
        }
    }
    return true;
}

std::string saveScreenshot(const std::vector<std::uint32_t>& pixels, int width, int height) {
    if (width <= 0 || height <= 0 || pixels.size() != static_cast<std::size_t>(width) * height) return {};
    const auto* home = std::getenv("HOME");
    const auto directory = fs::path(home && *home ? home : "/root") / "Pictures";
    std::error_code error;
    fs::create_directories(directory, error);
    if (error) return {};
    auto path = (directory / "Screen-XXXXXX.ppm").string();
    const int fd = mkstemps(path.data(), 4);
    if (fd < 0) return {};
    FILE* output = fdopen(fd, "wb");
    if (!output) {
        close(fd);
        unlink(path.c_str());
        return {};
    }
    std::vector<unsigned char> rgb;
    rgb.reserve(pixels.size() * 3);
    for (auto pixel : pixels) {
        rgb.push_back(pixel >> 16);
        rgb.push_back(pixel >> 8);
        rgb.push_back(pixel);
    }
    const bool written = fprintf(output, "P6\n%d %d\n255\n", width, height) > 0 &&
                         fwrite(rgb.data(), 1, rgb.size(), output) == rgb.size();
    if (fclose(output) != 0 || !written) {
        unlink(path.c_str());
        return {};
    }
    return path;
}
}
