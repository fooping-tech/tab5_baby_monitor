#include "live_services.hpp"
#include "detector.hpp"
#include "presence.hpp"
#include "frame_convert.hpp"
#include "hal/hal_uvc.h"
#include "jpeg_decoder.hpp"
#include "rtsp/rtsp_server.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include <mutex>

namespace {
std::mutex result_mutex;
baby_edge::PresenceFilter presence;
uint32_t result_generation = 0;
constexpr size_t payload_capacity = 1024 * 1024;
constexpr size_t rgb_capacity = CONFIG_TAB5_UVC_WIDTH * CONFIG_TAB5_UVC_HEIGHT * 3;

void infer_work(void *argument)
{
    constexpr size_t kModelContextMinimum = 3 * 1024 * 1024;
    if (heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM | MALLOC_CAP_SIMD) < kModelContextMinimum) {
        ESP_LOGE("edge_ai", "insufficient contiguous PSRAM for model; presence remains unknown");
        return;
    }
    baby_edge::PersonDetector detector(static_cast<const uint8_t *>(argument));
    auto *payload = static_cast<uint8_t *>(heap_caps_malloc(payload_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *rgb = edge_jpeg_rgb_buffer_alloc(rgb_capacity);
    if (!payload || !rgb) {
        heap_caps_free(payload);
        heap_caps_free(rgb);
        ESP_LOGE("edge_ai", "AI buffer allocation failed; presence remains unknown");
        return;
    }
    uint32_t sequence = 0;
    while (true) {
        hal_uvc_frame_info_t info{};
        if (hal_uvc_copy_latest_frame(sequence, payload, payload_capacity, &info)) {
            sequence = info.sequence;
            const int64_t captured_ms = info.captured_us / 1000;
            baby_edge::Detection result{};
            if (info.width > 0 && info.height > 0 && info.width <= CONFIG_TAB5_UVC_WIDTH &&
                info.height <= CONFIG_TAB5_UVC_HEIGHT && esp_timer_get_time() / 1000 - captured_ms <= 1000) {
                uint8_t *decoded = nullptr;
                if (info.format == HAL_UVC_FRAME_MJPEG) {
                    if (edge_decode_jpeg_rgb888(payload, info.bytes, info.width, info.height, rgb, rgb_capacity)) decoded = rgb;
                } else if (info.format == HAL_UVC_FRAME_YUY2 &&
                           baby_edge::yuy2_to_rgb(payload, info.bytes, info.width, info.height, info.stride, rgb, rgb_capacity)) {
                    decoded = rgb;
                }
                if (decoded) {
                    result = detector.detect({decoded, static_cast<size_t>(info.width) * info.height * 3,
                                              static_cast<uint16_t>(info.width), static_cast<uint16_t>(info.height), captured_ms});
                }
            }
            {
                std::lock_guard<std::mutex> lock(result_mutex);
                if (result_generation != info.generation) presence.reset();
                result_generation = info.generation;
                presence.update(captured_ms, esp_timer_get_time() / 1000,
                                result.valid && hal_uvc_is_streaming() && info.generation == hal_uvc_generation(),
                                result.person_score);
            }
            ESP_LOGI("edge_ai", "sequence=%lu valid=%d score=%.3f pipeline_us=%lld stack=%u",
                     static_cast<unsigned long>(sequence), result.valid, result.person_score,
                     static_cast<long long>(result.inference_us), static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
        }
        vTaskDelay(pdMS_TO_TICKS(CONFIG_EDGE_AI_INTERVAL_MS));
    }
}

void infer(void *argument)
{
    infer_work(argument);
    vTaskDeleteWithCaps(nullptr);
}
}

void edge_live_start(const unsigned char *model_data)
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
        ESP_LOGW("edge_ai", "UVC did not start before RTSP preparation; RTSP will retry on DESCRIBE");
    }
    ESP_ERROR_CHECK(edge_jpeg_decoder_start());
    ESP_ERROR_CHECK(edge_ui_start());
    ESP_ERROR_CHECK(edge_network_start());
    // Priority 1, below the preview consumer: one yolo26n pass is a single
    // non-preemptible ESP-DL call of 2.4-7.0 s and the interval between passes
    // is only 1 s, so this task is busy roughly 80% of the time. At equal
    // priority it starved the 5 Hz preview into blanking the camera image.
    // The interval stays at 1 s because PresenceFilter resets when frames fall
    // more than stale_ms (10 s) apart, and a slow pass plus a longer pause
    // would cross that. Left unpinned: core 1 belongs to the camera tasks.
    if (xTaskCreateWithCaps(infer, "edge_ai", 12 * 1024, const_cast<unsigned char *>(model_data), 1,
                            nullptr, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) != pdPASS) {
        ESP_LOGE("edge_ai", "AI task unavailable; presence remains unknown");
    }
    while (true) {
        if (!hal_uvc_is_capturing()) hal_uvc_start();
        baby_edge::Presence state;
        {
            std::lock_guard<std::mutex> lock(result_mutex);
            if (!hal_uvc_is_streaming() || result_generation != hal_uvc_generation()) presence.reset();
            state = presence.current(esp_timer_get_time() / 1000);
        }
        ESP_LOGI("edge_status", "presence=%s posture=unknown usb=%d free_psram=%u",
                 baby_edge::name(state), hal_uvc_is_streaming(),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));
        edge_ui_status(baby_edge::name(state));
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
