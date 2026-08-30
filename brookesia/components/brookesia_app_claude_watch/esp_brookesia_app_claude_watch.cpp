#include "lvgl.h"
#include "esp_brookesia.hpp"
#ifdef ESP_UTILS_LOG_TAG
#   undef ESP_UTILS_LOG_TAG
#endif
#define ESP_UTILS_LOG_TAG "BS:ClaudeWatch"
#include "esp_lib_utils.h"
#include "esp_brookesia_app_claude_watch.hpp"
#include "ui/cw_ui.h"

#define APP_NAME "Claude"

using namespace esp_brookesia::gui;
using namespace esp_brookesia::systems;

LV_IMAGE_DECLARE(icon_claude_112);

namespace esp_brookesia::apps {

ClaudeWatch *ClaudeWatch::_instance = nullptr;

ClaudeWatch *ClaudeWatch::requestInstance(bool use_status_bar, bool use_navigation_bar)
{
    if (_instance == nullptr) {
        _instance = new ClaudeWatch(use_status_bar, use_navigation_bar);
    }
    return _instance;
}

ClaudeWatch::ClaudeWatch(bool use_status_bar, bool use_navigation_bar):
    App(APP_NAME, &icon_claude_112, true /* default screen */, use_status_bar, use_navigation_bar)
{
}

ClaudeWatch::~ClaudeWatch() {}

bool ClaudeWatch::run(void)
{
    ESP_UTILS_LOGD("Run");
    // Everything (objects, the refresh timer, animations) is created on the default screen while
    // resources are being recorded, so the core cleans it all up when the app closes.
    cw_ui_create(lv_screen_active());
    return true;
}

bool ClaudeWatch::back(void)
{
    ESP_UTILS_LOGD("Back");
    ESP_UTILS_CHECK_FALSE_RETURN(notifyCoreClosed(), false, "Notify core closed failed");
    return true;
}

bool ClaudeWatch::close(void)
{
    ESP_UTILS_LOGD("Close");
    cw_ui_detach();   // forget object pointers; the core deletes the screen and timer
    return true;
}

ESP_UTILS_REGISTER_PLUGIN_WITH_CONSTRUCTOR(systems::base::App, ClaudeWatch, APP_NAME, []()
{
    return std::shared_ptr<ClaudeWatch>(ClaudeWatch::requestInstance(), [](ClaudeWatch * p) {});
})

} // namespace esp_brookesia::apps
