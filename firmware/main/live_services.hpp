#pragma once
#include "esp_err.h"

// hal_uvc.cpp already reserves core 1 for the camera path: the UVC driver task
// (priority 6), uvc_stream (5) and uvc_render (4) are all pinned there.
// Keeping RTSP on core 0 keeps the network and encoder work off that core.
// The preview worker stays unpinned so the scheduler can keep it off
// whichever core is busy; core 1 already carries three camera tasks at
// higher priority than it.
#define EDGE_SERVICE_CORE 0

esp_err_t edge_board_start();
esp_err_t edge_ui_start();
void edge_ui_status();
esp_err_t edge_network_start();
// Applies CONFIG_EDGE_TIMEZONE. Must run before any task formats local time,
// otherwise the display shows UTC until Wi-Fi associates.
void edge_clock_init();
bool edge_clock_is_synchronized();
void edge_live_start();
