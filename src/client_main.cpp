// SPDX-License-Identifier: GPL-3.0-only
// Adapted from xfangfang/wiliwili v1.6.0, wiliwili/source/main.cpp.
#include <borealis.hpp>
#include <borealis/platforms/desktop/desktop_platform.hpp>
#include "utils/activity_helper.hpp"
#include "utils/config_helper.hpp"
#include "view/mpv_core.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

int runClient(int argc, char** argv, const std::string& bvid) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-d") brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        else if (arg == "-v") brls::Application::enableDebuggingView(true);
        else if (arg == "-t") MPVCore::TERMINAL = true;
        else if (arg == "-o") {
            auto* file = std::fopen(i + 1 < argc ? argv[++i] : "wiliwili.log", "w");
            if (!file) throw std::runtime_error("Cannot open log file");
            brls::Logger::setLogOutput(file);
        } else throw std::runtime_error("Unknown option: " + arg);
    }
    auto& config = ProgramConfig::instance();
    config.init();
    const auto fullscreen = SettingItem::PLAYER_AUTO_FULLSCREEN;
    config.setSettingItem(fullscreen, config.getSettingItem(fullscreen, true), false);
    if (!brls::Application::init()) return 1;
    brls::Application::getPlatform()->exitToHomeMode(true);
    brls::Application::createWindow("wiliwili for CardputerZero（社区移植版）");
    Register::initCustomView();
    Register::initCustomTheme();
    Register::initCustomStyle();
    brls::Application::getPlatform()->disableScreenDimming(false);
    Intent::openMain();
    if (!bvid.empty()) Intent::openBV(bvid);
    while (brls::Application::mainLoop()) {}

    const bool restart = brls::DesktopPlatform::RESTART_APP;
    brls::DesktopPlatform::RESTART_APP = false;
    ProgramConfig::instance().exit(argv);
    return restart ? 75 : 0;
}
