#pragma once

#include <string>
#include <vector>

#include "../app_state.h"

namespace su::app {

struct PageCardContent {
    std::string title;
    std::string body;
};

struct PageContent {
    PageId id;
    std::string title;
    std::string description;
    PageCardContent primary_card;
    PageCardContent secondary_card;
    std::string stage_title;
    std::string stage_body;
};

PageContent make_page_content(PageId page);

}  // namespace su::app
