#include "detector.hpp"
#include "presence.hpp"
#include "dl_image_jpeg.hpp"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

extern const uint8_t model_start[] asm("_binary_person_model_espdl_start");
extern const uint8_t image_start[] asm("_binary_poc_image_jpg_start");
extern const uint8_t image_end[] asm("_binary_poc_image_jpg_end");

extern "C" void app_main()
{
    ESP_LOGW("baby_edge", "Still-image PoC only; person is not baby identity. Not a safety monitor.");
    baby_edge::PresenceFilter presence;
    baby_edge::PersonDetector detector(model_start);
    dl::image::jpeg_img_t jpeg{};
    jpeg.data = const_cast<uint8_t *>(image_start);
    jpeg.data_len = image_end - image_start;
    auto decoded = dl::image::sw_decode_jpeg(jpeg, dl::image::DL_IMAGE_PIX_TYPE_RGB888);
    if (!decoded.data) {
        ESP_LOGE("baby_edge", "decode failed: presence=unknown posture=unknown");
        return;
    }
    const auto captured_ms = esp_timer_get_time() / 1000;
    const baby_edge::RgbFrame frame{static_cast<uint8_t *>(decoded.data), decoded.bytes(),
                                     decoded.width, decoded.height, captured_ms};
    const auto result = detector.detect(frame);
    const auto state = presence.update(captured_ms, esp_timer_get_time() / 1000,
                                       result.valid, result.person_score);
    ESP_LOGI("baby_edge", "valid=%d person_score=%.3f pipeline_us=%lld presence=%s posture=unknown free_heap=%u",
             result.valid, result.person_score, static_cast<long long>(result.inference_us),
             baby_edge::name(state), static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_8BIT)));
    heap_caps_free(decoded.data);
}
