// SPDX-License-Identifier: GPL-3.0-only
// Adapted from xfangfang/wiliwili v1.6.0, wiliwili/source/main.cpp.
#include <borealis.hpp>
#include <borealis/platforms/desktop/desktop_platform.hpp>
#include "utils/activity_helper.hpp"
#include "utils/config_helper.hpp"
#include "utils/shortcut_helper.hpp"
#include "view/mpv_core.hpp"
#include "playback-display.hpp"
#include <tinyxml2.h>
#include <array>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>

namespace {
struct DisplayMetric {
    const char* name;
    float compact;
    float wide;
};

std::array<DisplayMetric, 7> displayMetrics() {
    auto style = brls::getStyle();
    std::array<DisplayMetric, 7> metrics{{
        {"wiliwili/grid/span/5", 2}, {"wiliwili/grid/span/4", 2}, {"wiliwili/grid/span/3", 2},
        {"wiliwili/grid/span/2", 1}, {"wiliwili/player/width", 312},
        {"wiliwili/tab_frame/content_padding_top_bottom", 4}, {"brls/tab_frame/content_padding_sides", 8},
    }};
    for (auto& metric : metrics) metric.wide = style.getMetric(metric.name);
    return metrics;
}

// Both video-control layouts have the same hierarchy. Apply only changed geometry
// so the existing widgets keep their actions, focus, text and playback state.
void applyVideoGeometry(brls::View& view, const tinyxml2::XMLElement& wide,
                        const tinyxml2::XMLElement& compact, bool lcd) {
    constexpr const char* attributes[]{
        "width", "height", "marginLeft", "marginRight", "marginTop", "marginBottom",
        "positionTop", "positionRight", "fontSize",
    };
    for (const auto* name : attributes) {
        const char* a = wide.Attribute(name);
        const char* b = compact.Attribute(name);
        if ((!a && !b) || (a && b && std::strcmp(a, b) == 0)) continue;
        if (const auto* value = lcd ? b : a) view.applyXMLAttribute(name, value);
        else if (std::strcmp(name, "fontSize") == 0)
            view.applyXMLAttribute(name, "@style/brls/label/default_font_size");
        else if (std::strncmp(name, "margin", 6) == 0)
            view.applyXMLAttribute(name, "0");
    }

    auto* box = dynamic_cast<brls::Box*>(&view);
    if (!box) return;
    const auto* a = wide.FirstChildElement();
    const auto* b = compact.FirstChildElement();
    for (auto* child : box->getChildren()) {
        if (!a || !b || std::strcmp(a->Name(), b->Name())) break;
        const char* id = a->Attribute("id");
        if (!id || child->getView(id) == child) applyVideoGeometry(*child, *a, *b, lcd);
        a = a->NextSiblingElement();
        b = b->NextSiblingElement();
    }
}
}

void cm0::setClientDisplayLayout(bool lcd) {
    static const auto metrics = displayMetrics();
    static const std::string widePath = brls::View::CUSTOM_RESOURCES_PATH.empty()
        ? std::string(BRLS_RESOURCES) + "hdmi/" : brls::View::CUSTOM_RESOURCES_PATH;
    auto style = brls::getStyle();
    for (const auto& metric : metrics) style.addMetric(metric.name, lcd ? metric.compact : metric.wide);
    brls::View::CUSTOM_RESOURCES_PATH = lcd ? std::string(BRLS_RESOURCES) + "lcd/" : widePath;
    std::fprintf(stderr, "layout: %s\n", lcd ? "LCD" : "HDMI");
}

void cm0::refreshVideoLayout(brls::View& video, bool lcd) {
    constexpr auto resource = "xml/views/video_view.xml";
    const auto load = [](const std::string& path) {
        auto document = brls::View::getXMLCache(path);
        if (!document->RootElement()) document->LoadFile(path.c_str());
        return document;
    };
    const auto wide = load(std::string(BRLS_RESOURCES) + resource);
    const auto compact = load(std::string(BRLS_RESOURCES) + "lcd/" + resource);
    if (wide->RootElement() && compact->RootElement()) {
        applyVideoGeometry(video, *wide->RootElement(), *compact->RootElement(), lcd);
        std::fprintf(stderr, "playback controls: %s\n", lcd ? "LCD" : "HDMI");
    }
}

int runClient(int argc, char** argv, const std::string& bvid) {
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-d") brls::Logger::setLogLevel(brls::LogLevel::LOG_DEBUG);
        else if (arg == "-v") brls::Application::enableDebuggingView(true);
        else if (arg == "-t") MPVCore::TERMINAL = true;
        else if (arg == "-o") {
            auto* file = std::fopen(i + 1 < argc ? argv[++i] : "wiliwili-cpz.log", "w");
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
    brls::Application::createWindow("wiliwili-cpz");
    Register::initCustomView();
    Register::initCustomTheme();
    brls::Theme::getLightTheme().addColor("font/secondary", nvgRGB(76, 83, 91));
    brls::Theme::getDarkTheme().addColor("font/secondary", nvgRGB(194, 199, 205));
    Register::initCustomStyle();
    brls::Application::getPlatform()->disableScreenDimming(false);
    if (cm0::compactLayout()) {
        // Fn lock maps the physical 5 key to F5.
        if (ShortcutHelper::getSetting() == brls::BRLS_KBD_KEY_F5)
            ShortcutHelper::setSetting("");
        ShortcutHelper::setRefresh("f5");
    }
    cm0::setClientDisplayLayout(cm0::compactLayout());
    Intent::openMain();
    if (!bvid.empty()) Intent::openBV(bvid);
    while (brls::Application::mainLoop()) {}

    const bool restart = brls::DesktopPlatform::RESTART_APP;
    brls::DesktopPlatform::RESTART_APP = false;
    ProgramConfig::instance().exit(argv);
    return restart ? 75 : 0;
}
