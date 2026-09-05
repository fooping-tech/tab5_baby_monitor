#include "live_services.hpp"
#include "detector.hpp"
#include "presence.hpp"
#include "frame_convert.hpp"
#include "hal/hal_uvc.h"
#include "dl_image_jpeg.hpp"
#include "driver/jpeg_decode.h"
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
constexpr size_t payload_capacity = 2 * 1024 * 1024;
constexpr size_t rgb_capacity = CONFIG_TAB5_UVC_WIDTH * CONFIG_TAB5_UVC_HEIGHT * 3;

void infer_work(void *argument)
{
    baby_edge::PersonDetector detector(static_cast<const uint8_t *>(argument));
    auto *payload = static_cast<uint8_t *>(heap_caps_malloc(payload_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    auto *rgb = static_cast<uint8_t *>(heap_caps_malloc(rgb_capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
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
                dl::image::img_t decoded{};
                if (info.format == HAL_UVC_FRAME_MJPEG) {
                    jpeg_decode_picture_info_t picture{};
                    if (jpeg_decoder_get_info(payload, info.bytes, &picture) == ESP_OK &&
                        picture.width == info.width && picture.height == info.height) {
                        dl::image::jpeg_img_t jpeg{};
                        jpeg.data = payload;
                        jpeg.data_len = info.bytes;
                        decoded = dl::image::sw_decode_jpeg(jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
                    }
                } else if (info.format == HAL_UVC_FRAME_YUY2 &&
                           baby_edge::yuy2_to_rgb(payload, info.bytes, info.width, info.height, info.stride, rgb, rgb_capacity)) {
                    decoded = {rgb, static_cast<uint16_t>(info.width), static_cast<uint16_t>(info.height),
                               dl::image::DL_IMAGE_PIX_TYPE_RGB888};
                }
                if (decoded.data && decoded.width == info.width && decoded.height == info.height) {
                    result = detector.detect({static_cast<uint8_t *>(decoded.data), decoded.bytes(),
                                              decoded.width, decoded.height, captured_ms});
                }
                if (decoded.data != rgb) heap_caps_free(decoded.data);
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
    ESP_ERROR_CHECK(edge_board_start());
    ESP_ERROR_CHECK(hal_uvc_init() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(hal_uvc_start() ? ESP_OK : ESP_FAIL);
    ESP_ERROR_CHECK(edge_network_start());
    if (xTaskCreateWithCaps(infer, "edge_ai", 32 * 1024, const_cast<unsigned char *>(model_data), 2,
                            nullptr, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) != pdPASS) {
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
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
