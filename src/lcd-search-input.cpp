// SPDX-License-Identifier: GPL-3.0-only
#include "lcd-search-input.hpp"
#include <algorithm>
#include <utility>
#include <borealis/core/application.hpp>
#include "utils/activity_helper.hpp"
#include "view/svg_image.hpp"

namespace cm0 {
using namespace brls::literals;

LcdSearchActivity::LcdSearchActivity(std::string initial) : query(std::move(initial)) {}

void LcdSearchActivity::displayQuery() {
    input->setText(query.empty() ? "wiliwili/search/tv/hint"_i18n : query);
    input->setTextColor(brls::Application::getTheme().getColor(query.empty() ? "font/grey" : "brls/text"));
}

void LcdSearchActivity::onContentAvailable() {
    const bool light = brls::Application::getThemeVariant() == brls::ThemeVariant::LIGHT;
    static_cast<SVGImage*>(getView("search/icon"))->setImageFromSVGRes(
        light ? "svg/header-search-dark.svg" : "svg/header-search.svg");
    field->registerClickAction([this](brls::View*) {
        submit(query);
        return true;
    });
    selection.subscribe([this](const std::string& value) { submit(value); });
    hot->setSearchCallback(&selection);
    history->setSearchCallback(&selection);
    history->requestHistory();
    displayQuery();
    setSearchTyping(true);
}

void LcdSearchActivity::submit(const std::string& value) {
    if (value.find_first_not_of(' ') == std::string::npos) return;
    query = value;
    displayQuery();
    brls::Application::giveFocus(field);
    refreshOnReturn = true;
    Intent::openSearch(query);
}

void LcdSearchActivity::type(char ch) {
    // Hot and history selections can contain multi-byte UTF-8 characters.
    const auto count = std::count_if(query.begin(), query.end(), [](unsigned char c) { return (c & 0xc0) != 0x80; });
    if (count >= 32) return;
    query.push_back(ch);
    brls::Application::giveFocus(field);
    displayQuery();
}

void LcdSearchActivity::backspace() {
    if (query.empty()) return;
    auto pos = query.size() - 1;
    while (pos && (static_cast<unsigned char>(query[pos]) & 0xc0) == 0x80) --pos;
    query.erase(pos);
    brls::Application::giveFocus(field);
    displayQuery();
}

void LcdSearchActivity::onPause() { setSearchTyping(false); }
void LcdSearchActivity::onResume() {
    setSearchTyping(true);
    if (refreshOnReturn) history->requestHistory();
    refreshOnReturn = false;
}
void LcdSearchActivity::willDisappear(bool resetState) {
    setSearchTyping(false);
    brls::Activity::willDisappear(resetState);
}

void LcdSearchActivity::edit(const std::string& value) {
    const auto stack = brls::Application::getActivitiesStack();
    if (stack.size() >= 2 && dynamic_cast<LcdSearchActivity*>(stack[stack.size() - 2])) {
        brls::Application::popActivity(brls::TransitionAnimation::NONE);
    } else {
        brls::Application::pushActivity(new LcdSearchActivity(value), brls::TransitionAnimation::NONE);
    }
}
}
