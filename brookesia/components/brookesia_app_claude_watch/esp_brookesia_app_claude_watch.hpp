#pragma once

#include "systems/phone/esp_brookesia_phone_app.hpp"

namespace esp_brookesia::apps {

/**
 * ClaudeWatch: watch face + live Claude Code status, as a Brookesia phone app.
 * Swipe left/right to flip between the two pages; swipe up from the bottom edge (system gesture) to leave.
 */
class ClaudeWatch: public systems::phone::App {
public:
    static ClaudeWatch *requestInstance(bool use_status_bar = false, bool use_navigation_bar = false);
    ~ClaudeWatch();

protected:
    ClaudeWatch(bool use_status_bar, bool use_navigation_bar);

    bool run(void) override;
    bool back(void) override;
    bool close(void) override;

private:
    static ClaudeWatch *_instance;
};

} // namespace esp_brookesia::apps
