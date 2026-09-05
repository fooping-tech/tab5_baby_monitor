#include "jpeg_decoder.hpp"

#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "hal/cache_hal.h"
#include "hal/cache_ll.h"

namespace {
jpeg_decoder_handle_t decoder = nullptr;
SemaphoreHandle_t decoder_lock = nullptr;
}

esp_err_t edge_jpeg_decoder_start()
{
    if (decoder) return ESP_OK;
    decoder_lock = xSemaphoreCreateMutex();
    if (!decoder_lock) return ESP_ERR_NO_MEM;
    jpeg_decode_engine_cfg_t config = {
        .intr_priority = 0,
        .timeout_ms = 200,
    };
    const esp_err_t result = jpeg_new_decoder_engine(&config, &decoder);
    if (result != ESP_OK) {
        vSemaphoreDelete(decoder_lock);
        decoder_lock = nullptr;
    }
    return result;
}

uint8_t *edge_jpeg_rgb_buffer_alloc(size_t rgb_capacity)
{
    const size_t alignment = cache_hal_get_cache_line_size(CACHE_LL_LEVEL_EXT_MEM, CACHE_TYPE_DATA);
    const size_t aligned_capacity = (rgb_capacity + alignment - 1) / alignment * alignment;
    return static_cast<uint8_t *>(heap_caps_aligned_calloc(alignment, 1, aligned_capacity,
                                                           MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
}

bool edge_decode_jpeg_rgb888(const uint8_t *jpeg, size_t jpeg_size, uint32_t width, uint32_t height,
                             uint8_t *rgb, size_t rgb_capacity)
{
    if (!decoder || !decoder_lock || !jpeg || !rgb || !width || !height || jpeg_size > UINT32_MAX ||
        width % 16 != 0 || height % 16 != 0 || width * height > rgb_capacity / 3) {
        return false;
    }
    jpeg_decode_picture_info_t picture{};
    if (jpeg_decoder_get_info(jpeg, jpeg_size, &picture) != ESP_OK || picture.width != width || picture.height != height) {
        return false;
    }
    if (xSemaphoreTake(decoder_lock, pdMS_TO_TICKS(250)) != pdTRUE) return false;
    const jpeg_decode_cfg_t config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_RGB,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t output_size = 0;
    const bool decoded = jpeg_decoder_process(decoder, &config, jpeg, jpeg_size, rgb, rgb_capacity, &output_size) == ESP_OK &&
                         output_size <= rgb_capacity;
    xSemaphoreGive(decoder_lock);
    return decoded;
}
