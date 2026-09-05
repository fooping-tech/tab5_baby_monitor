#pragma once

#include <cstddef>
#include <cstdint>
#include "esp_err.h"

esp_err_t edge_jpeg_decoder_start();
uint8_t *edge_jpeg_rgb_buffer_alloc(size_t rgb_capacity);
bool edge_decode_jpeg_rgb888(const uint8_t *jpeg, size_t jpeg_size, uint32_t width, uint32_t height,
                             uint8_t *rgb, size_t rgb_capacity);
