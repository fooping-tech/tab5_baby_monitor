#include "esp_log.h"
#include "live_services.hpp"
#include "nvs_flash.h"

extern "C" void app_main()
{
    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_LOGI("tab5_camera_clock", "USB UVC preview, H.264 RTSP and local clock");
    edge_live_start();
}
