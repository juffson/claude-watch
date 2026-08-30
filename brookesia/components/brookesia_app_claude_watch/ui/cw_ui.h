#pragma once
#include "lvgl.h"

// Build the ClaudeWatch UI (both pages + refresh timer) inside `parent` (the app screen).
// Must be called from the LVGL task while the app is recording resources (App::run()).
void cw_ui_create(lv_obj_t *parent);

// Called when the app closes: drop object pointers (the core deletes the objects/timer).
void cw_ui_detach(void);
