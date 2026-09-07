#include "live_services.hpp"
#include "hal/hal_uvc.h"
#include "jpeg_decoder.hpp"
#include "rtsp/rtsp_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

void edge_live_start()
{
    // Before the UI or any logging formats local time.
    edge_clock_init();
    ESP_ERROR_CHECK(edge_board_start());
    ESP_ERROR_CHECK(hal_uvc_init() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(hal_uvc_start() ? ESP_OK : ESP_FAIL);
    for (int retry = 0; retry < 50 && !hal_uvc_is_streaming(); ++retry) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (hal_uvc_is_streaming()) {
        ESP_ERROR_CHECK(rtsp_server_prepare());
    } else {
        ESP_LOGW("tab5_camera", "UVC did not start before RTSP preparation; RTSP will retry on DESCRIBE");
    }
    ESP_ERROR_CHECK(edge_jpeg_decoder_start());
    ESP_ERROR_CHECK(edge_ui_start());
    ESP_ERROR_CHECK(edge_network_start());
    while (true) {
        if (!hal_uvc_is_capturing()) hal_uvc_start();
        ESP_LOGI("tab5_status", "usb=%d free_psram=%u", hal_uvc_is_streaming(),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        edge_ui_status();
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
