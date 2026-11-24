#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace cm0 {

struct KeyEvent {
    unsigned code;
    bool down;
};

class Keyboard {
public:
    Keyboard();
    ~Keyboard();
    std::vector<KeyEvent> poll();

private:
    struct State;
    std::unique_ptr<State> state_;
};

class Display {
public:
    explicit Display(std::string choice);
    ~Display();
    bool update();
    void present(const std::vector<std::uint32_t>& rgb, int width, int height);
    bool usingHdmi() const;

private:
    struct State;
    std::unique_ptr<State> state_;
};

int supervise(const std::function<int()>& client);
bool stopping();
bool hdmiAudioAvailable();
bool systemHotkey(unsigned code, bool localDisplay, std::string* feedback);
std::string saveScreenshot(const std::vector<std::uint32_t>& pixels, int width, int height);

}
