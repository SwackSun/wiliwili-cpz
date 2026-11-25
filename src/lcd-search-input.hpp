// SPDX-License-Identifier: GPL-3.0-only
#pragma once

#include <borealis/core/activity.hpp>
#include <borealis/core/bind.hpp>
#include <borealis/views/label.hpp>
#include <string>
#include "fragment/search_history.hpp"
#include "fragment/search_hots.hpp"

namespace cm0 {

class LcdSearchActivity : public brls::Activity {
public:
    CONTENT_FROM_XML_RES("activity/search_activity_tv.xml");
    explicit LcdSearchActivity(std::string initial = {});
    void onContentAvailable() override;
    void onPause() override;
    void onResume() override;
    void willDisappear(bool resetState) override;
    void type(char ch);
    void backspace();
    static void edit(const std::string& query);

private:
    void displayQuery();
    void submit(const std::string& value);

    std::string query;
    bool refreshOnReturn = false;
    brls::Event<std::string> selection;
    BRLS_BIND(brls::Label, input, "search/input");
    BRLS_BIND(brls::Box, field, "search/field");
    BRLS_BIND(SearchHots, hot, "search/hots");
    BRLS_BIND(SearchHistory, history, "search/history");
};

void setSearchTyping(bool active);

}
