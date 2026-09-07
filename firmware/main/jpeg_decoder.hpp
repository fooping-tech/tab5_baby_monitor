#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include "esp_err.h"

// Where preview/AI frames are lost, sampled once per second by edge_ui_status.
extern std::atomic<uint32_t> edge_jpeg_lock_timeouts;
extern std::atomic<uint32_t> edge_jpeg_decode_failures;

esp_err_t edge_jpeg_decoder_start();
uint8_t *edge_jpeg_rgb_buffer_alloc(size_t rgb_capacity);
bool edge_decode_jpeg_rgb888(const uint8_t *jpeg, size_t jpeg_size, uint32_t width, uint32_t height,
                             uint8_t *rgb, size_t rgb_capacity);

// True when the payload starts with SOI and still carries EOI. The ESP32-P4
// hardware decoder streams until it sees EOI, so a UVC frame truncated by an
// isochronous error (the "uvc-isoc: missed EoF" case) makes it wait for data
// that never arrives: jpeg_decoder_process() then only returns after its
// timeout and calls dma2d_force_end() on a transaction that is no longer in
// flight. Screen frames with this before handing them to the hardware.
bool edge_jpeg_payload_is_complete(const uint8_t *jpeg, size_t jpeg_size);
