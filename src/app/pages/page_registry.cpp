#include "page_content.h"

namespace su::app {

PageContent make_dashboard_page_content();
PageContent make_enrollment_page_content();
PageContent make_recognition_page_content();
PageContent make_settings_page_content();
PageContent make_status_page_content();

PageContent make_page_content(PageId page) {
    switch (page) {
        case PageId::Dashboard:
            return make_dashboard_page_content();
        case PageId::Enrollment:
            return make_enrollment_page_content();
        case PageId::Recognition:
            return make_recognition_page_content();
        case PageId::Settings:
            return make_settings_page_content();
        case PageId::Status:
            return make_status_page_content();
    }
    return make_dashboard_page_content();
}

}  // namespace su::app
