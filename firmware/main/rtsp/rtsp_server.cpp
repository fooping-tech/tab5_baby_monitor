/*
 * SPDX-License-Identifier: MIT
 *
 * The M5Stack UserDemo already provides the Tab5 MIPI-CSI camera through
 * esp_video. This file adds only the network-facing RTSP transport and uses
 * the official ESP32-P4 H.264 V4L2 M2M device instead of JPEG/RTP.
 */

#include "rtsp_server.h"

#include <atomic>
#include <errno.h>
#include <algorithm>
#include <fcntl.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>

#include "driver/jpeg_decode.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_init.h"
#include "hal/hal_uvc.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"
#include "linux/videodev2.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"
#include "rtsp_framing.h"

#ifndef MAP_FAILED
#define MAP_FAILED ((void *)-1)
#endif

namespace {

constexpr char kTag[]                  = "rtsp";
constexpr uint16_t kRtspPort           = 8554;
constexpr size_t kEncoderBufferCount   = 1;
constexpr size_t kMaxRtspRequest       = tab5::rtsp::kMaxRtspRequestBytes;
constexpr size_t kMaxRtpPayload        = 1400;
constexpr uint32_t kRtpClockRate       = 90000;
constexpr uint32_t kRtpSsrc            = 0x54414235; // "TAB5"
constexpr uint8_t kRtpPayloadType      = 96;
// Scrypted's FFmpeg rebroadcast can briefly stop reading while it hands an
// interleaved access unit to the next stage. Keep a bounded media-send grace
// period without allowing a dead peer to hold the RTSP task indefinitely.
constexpr uint32_t kSendDeadlineMs     = 3000;
// PLAY must produce a successful RTP packet at least once per this interval.
// This catches a stalled pipeline even when the peer socket has not reported
// an error and the send path therefore cannot trigger its own deadline.
constexpr int64_t kRtpPacketStallTimeoutUs = 3 * 1000 * 1000;
constexpr int kEncoderMinQp            = 20;
constexpr int kEncoderMaxQp            = 35;
constexpr uint32_t kMipiWidth          = 1280;
constexpr uint32_t kMipiHeight         = 720;
constexpr size_t kMaxUsbFrameBytes     = 2 * 1024 * 1024;
constexpr size_t kUsbRgb888Bytes       = static_cast<size_t>(CONFIG_TAB5_UVC_WIDTH) *
                                          CONFIG_TAB5_UVC_HEIGHT * 3;
constexpr size_t kUsbYuv420Bytes       = (static_cast<size_t>(CONFIG_TAB5_UVC_WIDTH) *
                                          CONFIG_TAB5_UVC_HEIGHT * 3) / 2;
constexpr int kUsbConversionRetryCount   = 3;
constexpr int kMaxConsecutiveFrameDrops  = 5;

enum class VideoSource : uint8_t {
    Mipi,
    UsbUvc,
};

enum class EncodeResult : uint8_t {
    Ready,
    Dropped,
    Failed,
};

enum class FrameResult : uint8_t {
    Sent,
    Dropped,
    Failed,
};

struct MmapBuffer {
    uint8_t* data = nullptr;
    size_t length = 0;
};

int s_listen_fd = -1;
bool s_server_started = false;
int s_encoder_fd = -1;
MmapBuffer s_encoder_buffers[kEncoderBufferCount];
size_t s_encoder_buffer_count = 0;
bool s_pipeline_ready = false;
enum class PipelineState { Idle, Starting, Running, Recovering, Failed };
PipelineState s_pipeline_state = PipelineState::Idle;
// The RTSP task is the sole owner of the legacy per-session counters below.
// Diagnostics reads only these atomics, which remain valid across client
// disconnects and therefore cannot race a new session's counter reset.
std::atomic<bool> s_diag_server_started{false};
std::atomic<bool> s_diag_pipeline_ready{false};
std::atomic<bool> s_diag_active_client{false};
std::atomic<uint8_t> s_diag_pipeline_state{static_cast<uint8_t>(PipelineState::Idle)};
std::atomic<uint8_t> s_diag_video_source{static_cast<uint8_t>(VideoSource::Mipi)};
std::atomic<uint64_t> s_lifetime_connections{0};
std::atomic<uint64_t> s_lifetime_access_units{0};
std::atomic<uint64_t> s_lifetime_packets{0};
std::atomic<uint64_t> s_lifetime_bytes{0};
std::atomic<uint64_t> s_lifetime_send_timeouts{0};
std::atomic<uint64_t> s_lifetime_socket_errors{0};
std::atomic<uint64_t> s_lifetime_encoder_timeouts{0};
std::atomic<uint64_t> s_lifetime_encoder_errors{0};
std::atomic<uint64_t> s_lifetime_recoveries{0};
std::atomic<uint64_t> s_lifetime_rtp_stall_watchdogs{0};
std::atomic<TaskHandle_t> s_rtsp_task{nullptr};
uint32_t s_send_timeouts = 0;
uint32_t s_send_socket_errors = 0;
uint32_t s_encoder_timeouts = 0;
uint32_t s_encoder_errors = 0;
uint32_t s_usb_conversion_errors = 0;
uint32_t s_rtcp_packets = 0;
uint64_t s_bytes_sent = 0;
uint32_t s_width = 1280;
uint32_t s_height = 720;
uint32_t s_rtp_timestamp = 0;
uint16_t s_rtp_sequence = 0;
uint8_t s_rtp_channel = 0;
uint8_t s_rtcp_channel = 1;
int64_t s_last_rtp_packet_us = 0;
uint8_t s_sps[256];
size_t s_sps_size = 0;
uint8_t s_pps[128];
size_t s_pps_size = 0;
uint32_t s_encoder_frame_sequence = 0;
uint32_t s_encode_debug_count = 0;
uint8_t* s_pending_idr = nullptr;
size_t s_pending_idr_size = 0;
uint32_t s_rtp_access_unit_debug_count = 0;
VideoSource s_video_source = VideoSource::Mipi;
uint8_t* s_usb_input = nullptr;
uint8_t* s_usb_rgb888 = nullptr;
uint8_t* s_usb_yuv420 = nullptr;
size_t s_usb_rgb888_capacity = 0;
size_t s_usb_yuv420_capacity = 0;
jpeg_decoder_handle_t s_usb_jpeg_decoder = nullptr;
uint32_t s_usb_sequence = 0;

static void set_pipeline_state(PipelineState state)
{
    s_pipeline_state = state;
    s_diag_pipeline_state.store(static_cast<uint8_t>(state), std::memory_order_release);
}

static void record_send_timeout()
{
    ++s_send_timeouts;
    s_lifetime_send_timeouts.fetch_add(1, std::memory_order_relaxed);
}

static void record_socket_error()
{
    ++s_send_socket_errors;
    s_lifetime_socket_errors.fetch_add(1, std::memory_order_relaxed);
}

static void record_encoder_timeout()
{
    ++s_encoder_timeouts;
    s_lifetime_encoder_timeouts.fetch_add(1, std::memory_order_relaxed);
}

static void record_encoder_error()
{
    ++s_encoder_errors;
    s_lifetime_encoder_errors.fetch_add(1, std::memory_order_relaxed);
}

static bool ioctl_ok(int fd, unsigned long request, void* arg, const char* operation)
{
    if (ioctl(fd, request, arg) == 0) {
        return true;
    }
    ESP_LOGE(kTag, "%s failed: errno=%d", operation, errno);
    return false;
}

static bool wait_fd_ready(int fd, bool writable, int timeout_ms)
{
    fd_set read_set;
    fd_set write_set;
    FD_ZERO(&read_set);
    FD_ZERO(&write_set);
    if (writable) {
        FD_SET(fd, &write_set);
    } else {
        FD_SET(fd, &read_set);
    }
    struct timeval timeout = {
        .tv_sec = timeout_ms / 1000,
        .tv_usec = (timeout_ms % 1000) * 1000,
    };
    return select(fd + 1, writable ? nullptr : &read_set, writable ? &write_set : nullptr,
                  nullptr, &timeout) > 0;
}

static bool set_pix_format(int fd, uint32_t type, uint32_t width, uint32_t height, uint32_t pixel_format)
{
    struct v4l2_format format = {};
    format.type = type;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = pixel_format;
    format.fmt.pix.field = V4L2_FIELD_NONE;
    return ioctl_ok(fd, VIDIOC_S_FMT, &format, "VIDIOC_S_FMT");
}

static bool map_buffers(int fd, uint32_t type, MmapBuffer* buffers, size_t requested, size_t* actual)
{
    *actual = 0;
    struct v4l2_requestbuffers request = {};
    request.count = requested;
    request.type = type;
    request.memory = V4L2_MEMORY_MMAP;
    if (!ioctl_ok(fd, VIDIOC_REQBUFS, &request, "VIDIOC_REQBUFS")) {
        return false;
    }
    if (request.count == 0 || request.count > requested) {
        ESP_LOGE(kTag, "unexpected V4L2 buffer count: %u", static_cast<unsigned int>(request.count));
        return false;
    }

    for (uint32_t i = 0; i < request.count; ++i) {
        struct v4l2_buffer buffer = {};
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (!ioctl_ok(fd, VIDIOC_QUERYBUF, &buffer, "VIDIOC_QUERYBUF")) {
            break;
        }
        void* mapped = mmap(nullptr, buffer.length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, buffer.m.offset);
        // The ESP-IDF esp_video mmap shim returns nullptr on failure and does
        // not provide the POSIX MAP_FAILED macro.
        if (mapped == nullptr || mapped == MAP_FAILED) {
            ESP_LOGE(kTag, "mmap failed: errno=%d", errno);
            break;
        }
        buffers[i].data = static_cast<uint8_t*>(mapped);
        buffers[i].length = buffer.length;
        *actual = i + 1;
    }
    if (*actual != request.count) {
        for (size_t i = 0; i < *actual; ++i) {
            munmap(buffers[i].data, buffers[i].length);
            buffers[i] = {};
        }
        *actual = 0;
        return false;
    }
    return true;
}

static void unmap_buffers(MmapBuffer* buffers, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        if (buffers[i].data == nullptr) {
            continue;
        }
        munmap(buffers[i].data, buffers[i].length);
        buffers[i] = {};
    }
}

static bool queue_mmap_buffers(int fd, uint32_t type, size_t count)
{
    for (size_t i = 0; i < count; ++i) {
        struct v4l2_buffer buffer = {};
        buffer.type = type;
        buffer.memory = V4L2_MEMORY_MMAP;
        buffer.index = i;
        if (!ioctl_ok(fd, VIDIOC_QBUF, &buffer, "VIDIOC_QBUF")) {
            return false;
        }
    }
    return true;
}

static bool stream_on(int fd, uint32_t type, const char *operation)
{
    return ioctl_ok(fd, VIDIOC_STREAMON, &type, operation);
}

static void stream_off(int fd, uint32_t type)
{
    if (fd >= 0) {
        ioctl(fd, VIDIOC_STREAMOFF, &type);
    }
}

static void quiesce_encoder_dma()
{
    // A USERPTR frame remains owned by the encoder until both queues are
    // stopped.  Keep this ordering in one helper so every error path drains
    // DMA before releasing the broker slot reference.
    stream_off(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    stream_off(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
}

static bool set_encoder_control(uint32_t id, int32_t value)
{
    struct v4l2_ext_control control = {};
    struct v4l2_ext_controls controls = {};
    control.id = id;
    control.value = value;
    controls.ctrl_class = V4L2_CID_CODEC_CLASS;
    controls.count = 1;
    controls.controls = &control;
    if (ioctl(s_encoder_fd, VIDIOC_S_EXT_CTRLS, &controls) == 0) {
        return true;
    }
    ESP_LOGW(kTag, "H.264 control 0x%" PRIx32 "=%" PRId32 " rejected: errno=%d", id, value, errno);
    return false;
}

static void shutdown_video(bool preserve_usb_source_buffers = false);

static bool fail_video_initialization(void)
{
    ESP_LOGE(kTag, "RTSP video initialization failed; cleaning up for retry");
    set_pipeline_state(PipelineState::Failed);
    shutdown_video();
    return false;
}

static void free_usb_source_buffers()
{
    if (s_usb_jpeg_decoder != nullptr) {
        jpeg_del_decoder_engine(s_usb_jpeg_decoder);
        s_usb_jpeg_decoder = nullptr;
    }
    if (s_usb_input != nullptr) {
        heap_caps_free(s_usb_input);
        s_usb_input = nullptr;
    }
    if (s_usb_rgb888 != nullptr) {
        heap_caps_free(s_usb_rgb888);
        s_usb_rgb888 = nullptr;
    }
    if (s_usb_yuv420 != nullptr) {
        heap_caps_free(s_usb_yuv420);
        s_usb_yuv420 = nullptr;
    }
    s_usb_rgb888_capacity = 0;
    s_usb_yuv420_capacity = 0;
    s_usb_sequence = 0;
}

static bool prepare_usb_source_buffers()
{
    if (s_usb_input != nullptr && s_usb_jpeg_decoder != nullptr && s_usb_rgb888 != nullptr &&
        s_usb_rgb888_capacity >= kUsbRgb888Bytes && s_usb_yuv420 != nullptr &&
        s_usb_yuv420_capacity >= kUsbYuv420Bytes) {
        return true;
    }
    // A partial allocation can only be left by an interrupted initialization.
    // Releasing that partial set before allocating prevents a stale pointer or
    // a leaked 2 MiB input buffer from poisoning the next attempt.
    if (s_usb_input != nullptr || s_usb_jpeg_decoder != nullptr || s_usb_rgb888 != nullptr ||
        s_usb_yuv420 != nullptr) {
        free_usb_source_buffers();
    }
    constexpr uint32_t kMemoryCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT | MALLOC_CAP_CACHE_ALIGNED;
    s_usb_input = static_cast<uint8_t *>(heap_caps_aligned_calloc(64, 1, kMaxUsbFrameBytes, kMemoryCaps));
    if (s_usb_input == nullptr) {
        ESP_LOGE(kTag, "USB RTSP input buffer allocation failed: bytes=%u PSRAM free=%u largest=%u",
                 static_cast<unsigned>(kMaxUsbFrameBytes),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM)));
        free_usb_source_buffers();
        return false;
    }

    // The RGB888 conversion plus P4 JPEG DMA can exceed 100 ms at 640x480
    // under Preview + RTSP load.  A timeout here is a frame-level failure,
    // not a reason to tear down the H.264 pipeline; allow the bounded frame
    // retry path enough time to receive a complete decode.
    const jpeg_decode_engine_cfg_t engine = {.intr_priority = 0, .timeout_ms = 500};
    if (jpeg_new_decoder_engine(&engine, &s_usb_jpeg_decoder) != ESP_OK) {
        ESP_LOGE(kTag, "USB RTSP JPEG decoder initialization failed");
        free_usb_source_buffers();
        return false;
    }
    const jpeg_decode_memory_alloc_cfg_t output_cfg = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    s_usb_rgb888 = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(kUsbRgb888Bytes, &output_cfg,
                                                                  &s_usb_rgb888_capacity));
    s_usb_yuv420 = static_cast<uint8_t *>(jpeg_alloc_decoder_mem(kUsbYuv420Bytes, &output_cfg,
                                                                  &s_usb_yuv420_capacity));
    if (s_usb_rgb888 == nullptr || s_usb_rgb888_capacity < kUsbRgb888Bytes ||
        s_usb_yuv420 == nullptr || s_usb_yuv420_capacity < kUsbYuv420Bytes) {
        ESP_LOGE(kTag, "USB RTSP decode buffer allocation failed: rgb888=%u/%u yuv420=%u/%u",
                 static_cast<unsigned>(kUsbRgb888Bytes), static_cast<unsigned>(s_usb_rgb888_capacity),
                 static_cast<unsigned>(kUsbYuv420Bytes), static_cast<unsigned>(s_usb_yuv420_capacity));
        free_usb_source_buffers();
        return false;
    }
    ESP_LOGI(kTag, "USB RTSP source buffers ready: input=%u rgb888=%u yuv420=%u",
             static_cast<unsigned>(kMaxUsbFrameBytes), static_cast<unsigned>(s_usb_rgb888_capacity),
             static_cast<unsigned>(s_usb_yuv420_capacity));
    return true;
}

static bool copy_usb_frame(hal_uvc_frame_info_t *info)
{
    if (info == nullptr || s_usb_input == nullptr) {
        return false;
    }
    const int64_t deadline = esp_timer_get_time() + 1000000;
    while (esp_timer_get_time() < deadline) {
        if (hal_uvc_copy_latest_frame(s_usb_sequence, s_usb_input, kMaxUsbFrameBytes, info)) {
            return true;
        }
        if (!hal_uvc_is_streaming()) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGW(kTag, "USB RTSP frame wait timed out after sequence=%" PRIu32, s_usb_sequence);
    return false;
}

static bool convert_usb_yuy2_to_yuv420(const hal_uvc_frame_info_t &info)
{
    if (s_usb_yuv420 == nullptr || info.width != s_width || info.height != s_height ||
        info.stride < info.width * 2 || info.bytes < static_cast<size_t>(info.stride) * info.height) {
        return false;
    }
    // The P4 H.264 device advertises V4L2_PIX_FMT_YUV420 but its hardware
    // encoder expects ESP_H264_RAW_FMT_O_UYY_E_VYY: every output row is
    // packed as C,Y,Y, with U on even rows and V on odd rows. A planar I420
    // buffer produces valid-looking bytes but incorrect colors/structure.
    for (uint32_t y = 0; y < info.height; ++y) {
        const uint8_t *row = s_usb_input + static_cast<size_t>(y) * info.stride;
        const uint32_t chroma_y = y & ~1U;
        const uint8_t *chroma_row = s_usb_input + static_cast<size_t>(chroma_y) * info.stride;
        const uint8_t *chroma_next = chroma_y + 1 < info.height
                                         ? s_usb_input + static_cast<size_t>(chroma_y + 1) * info.stride
                                         : chroma_row;
        const size_t output_row = static_cast<size_t>(y) * info.width * 3 / 2;
        for (uint32_t x = 0; x < info.width; x += 2) {
            const size_t source = static_cast<size_t>(x) * 2;
            const size_t destination = output_row + static_cast<size_t>(x / 2) * 3;
            const uint8_t chroma = static_cast<uint8_t>(
                ((y & 1U) == 0 ? static_cast<unsigned>(chroma_row[source + 1]) + chroma_next[source + 1]
                                : static_cast<unsigned>(chroma_row[source + 3]) + chroma_next[source + 3]) /
                2U);
            s_usb_yuv420[destination] = chroma;
            s_usb_yuv420[destination + 1] = row[source];
            s_usb_yuv420[destination + 2] = x + 1 < info.width ? row[source + 2] : row[source];
        }
    }
    return true;
}

static bool decode_usb_mjpeg_to_yuv420(const hal_uvc_frame_info_t &info)
{
    if (s_usb_jpeg_decoder == nullptr || s_usb_rgb888 == nullptr || s_usb_yuv420 == nullptr ||
        info.width != s_width || info.height != s_height) {
        return false;
    }
    jpeg_decode_picture_info_t picture = {};
    const esp_err_t info_result = jpeg_decoder_get_info(s_usb_input, info.bytes, &picture);
    if (info_result != ESP_OK || picture.width != info.width || picture.height != info.height) {
        ESP_LOGW(kTag, "USB MJPEG header rejected: err=%s jpeg=%ux%u frame=%" PRIu32 "x%" PRIu32 " bytes=%u",
                 esp_err_to_name(info_result), static_cast<unsigned>(picture.width),
                 static_cast<unsigned>(picture.height), info.width, info.height,
                 static_cast<unsigned>(info.bytes));
        return false;
    }
    // UVC cameras commonly emit YUV422 JPEGs.  ESP-IDF's JPEG decoder does
    // not support converting YUV422 input directly to YUV420 output, so use
    // its RGB888 path (which supports all JPEG sampling modes) and explicitly
    // pack the result into the P4 H.264 O_UYY_E_VYY layout below.
    const jpeg_decode_cfg_t config = {
        .output_format = JPEG_DECODE_OUT_FORMAT_RGB888,
        .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR,
        .conv_std = JPEG_YUV_RGB_CONV_STD_BT601,
    };
    uint32_t decoded = 0;
    const esp_err_t decode_result = jpeg_decoder_process(s_usb_jpeg_decoder, &config, s_usb_input, info.bytes,
                                                         s_usb_rgb888, s_usb_rgb888_capacity, &decoded);
    if (decode_result != ESP_OK || decoded < kUsbRgb888Bytes) {
        ESP_LOGW(kTag, "USB MJPEG decode rejected: err=%s decoded=%u expected=%u bytes=%u",
                 esp_err_to_name(decode_result), static_cast<unsigned>(decoded),
                 static_cast<unsigned>(kUsbRgb888Bytes), static_cast<unsigned>(info.bytes));
        return false;
    }

    auto clamp_byte = [](int value) -> uint8_t {
        return static_cast<uint8_t>(value < 0 ? 0 : value > 255 ? 255 : value);
    };
    auto yuv_from_bgr = [&](const uint8_t *pixel, uint8_t *y, uint8_t *u, uint8_t *v) {
        // JPEG's BT.601 CSC uses the limited-range integer coefficients. The
        // decoder is configured for BGR byte order, hence pixel[0..2] is B/G/R.
        const int b = pixel[0];
        const int g = pixel[1];
        const int r = pixel[2];
        *y = clamp_byte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        *u = clamp_byte(((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128);
        *v = clamp_byte(((112 * r - 94 * g - 18 * b + 128) >> 8) + 128);
    };

    // The P4 H.264 driver advertises V4L2_PIX_FMT_YUV420 but consumes rows
    // packed as C,Y,Y: U is stored on even rows and V on odd rows. Average
    // each 2x2 chroma block after decoding the JPEG to RGB888.
    for (uint32_t y = 0; y < info.height; y += 2) {
        const uint32_t next_y = y + 1 < info.height ? y + 1 : y;
        const size_t even_output_row = static_cast<size_t>(y) * info.width * 3 / 2;
        const size_t odd_output_row = static_cast<size_t>(next_y) * info.width * 3 / 2;
        for (uint32_t x = 0; x < info.width; x += 2) {
            const uint32_t next_x = x + 1 < info.width ? x + 1 : x;
            const uint8_t *p00 = s_usb_rgb888 + (static_cast<size_t>(y) * info.width + x) * 3;
            const uint8_t *p01 = s_usb_rgb888 + (static_cast<size_t>(y) * info.width + next_x) * 3;
            const uint8_t *p10 = s_usb_rgb888 + (static_cast<size_t>(next_y) * info.width + x) * 3;
            const uint8_t *p11 = s_usb_rgb888 + (static_cast<size_t>(next_y) * info.width + next_x) * 3;
            uint8_t y00, u00, v00, y01, u01, v01, y10, u10, v10, y11, u11, v11;
            yuv_from_bgr(p00, &y00, &u00, &v00);
            yuv_from_bgr(p01, &y01, &u01, &v01);
            yuv_from_bgr(p10, &y10, &u10, &v10);
            yuv_from_bgr(p11, &y11, &u11, &v11);
            const size_t even_destination = even_output_row + static_cast<size_t>(x / 2) * 3;
            const size_t odd_destination = odd_output_row + static_cast<size_t>(x / 2) * 3;
            s_usb_yuv420[even_destination] = static_cast<uint8_t>(
                (static_cast<unsigned>(u00) + u01 + u10 + u11) / 4U);
            s_usb_yuv420[even_destination + 1] = y00;
            s_usb_yuv420[even_destination + 2] = y01;
            s_usb_yuv420[odd_destination] = static_cast<uint8_t>(
                (static_cast<unsigned>(v00) + v01 + v10 + v11) / 4U);
            s_usb_yuv420[odd_destination + 1] = y10;
            s_usb_yuv420[odd_destination + 2] = y11;
        }
    }
    return true;
}

static bool initialize_video(void)
{
    if (s_pipeline_ready) {
        return true;
    }
    set_pipeline_state(PipelineState::Starting);
    if (!hal_uvc_is_streaming()) return fail_video_initialization();
    s_video_source = VideoSource::UsbUvc;
    s_diag_video_source.store(static_cast<uint8_t>(s_video_source), std::memory_order_release);
    s_width = s_video_source == VideoSource::UsbUvc ? CONFIG_TAB5_UVC_WIDTH : kMipiWidth;
    s_height = s_video_source == VideoSource::UsbUvc ? CONFIG_TAB5_UVC_HEIGHT : kMipiHeight;
    ESP_LOGI(kTag, "RTSP video source: %s %" PRIu32 "x%" PRIu32,
             s_video_source == VideoSource::UsbUvc ? "USB UVC" : "internal MIPI", s_width, s_height);

    ESP_LOGI(kTag, "H.264 memory before shared capture: internal free=%u largest=%u, PSRAM free=%u",
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    // esp_video_init() registers /dev/video11 and the MIPI device globally,
    // but does not start the shared CSI capture queue.  Register the devices
    // before opening H.264 while keeping the larger internal heap block
    // available; hal_camera_mipi_capture_acquire() below still performs the
    // actual MIPI buffer setup and STREAMON after H.264 has started.
    static bool video_registered = false;
    const esp_video_init_config_t video_config = {};
    if (!video_registered && esp_video_init(&video_config) != ESP_OK) {
        ESP_LOGE(kTag, "shared MIPI video devices could not be registered");
        return fail_video_initialization();
    }

    video_registered = true;

    // The P4 H.264 driver allocates a roughly 92 KiB contiguous internal
    // reference frame when the encoder queue is started.  Starting the shared
    // MIPI capture first consumes smaller internal blocks and can reduce the
    // largest free block below that threshold.  Initialize H.264 first, then
    // acquire the shared camera stream.  The capture task still owns the MIPI
    // stream; RTSP only consumes its latest YUV420 frame.
    ESP_LOGI(kTag, "H.264 memory before encoder open: internal free=%u largest=%u, PSRAM free=%u",
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned int>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    s_encoder_fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDWR | O_NONBLOCK);
    if (s_encoder_fd < 0) {
        ESP_LOGE(kTag, "cannot open %s; enable CONFIG_ESP_VIDEO_ENABLE_HW_H264_VIDEO_DEVICE", ESP_VIDEO_H264_DEVICE_NAME);
        return fail_video_initialization();
    }
    struct v4l2_capability encoder_capability = {};
    if (!ioctl_ok(s_encoder_fd, VIDIOC_QUERYCAP, &encoder_capability, "H.264 VIDIOC_QUERYCAP")) {
        return fail_video_initialization();
    }
    ESP_LOGI(kTag, "H.264 encoder driver=%s card=%s bus=%s", encoder_capability.driver, encoder_capability.card,
             encoder_capability.bus_info);

    set_encoder_control(V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, CONFIG_TAB5_RTSP_GOP);
    set_encoder_control(V4L2_CID_MPEG_VIDEO_BITRATE, CONFIG_TAB5_RTSP_BITRATE);
    set_encoder_control(V4L2_CID_MPEG_VIDEO_H264_MIN_QP, kEncoderMinQp);
    set_encoder_control(V4L2_CID_MPEG_VIDEO_H264_MAX_QP, kEncoderMaxQp);

    if (!set_pix_format(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, s_width, s_height, V4L2_PIX_FMT_YUV420)) {
        return fail_video_initialization();
    }
    struct v4l2_requestbuffers encoder_input_request = {};
    encoder_input_request.count = 1;
    encoder_input_request.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    encoder_input_request.memory = V4L2_MEMORY_USERPTR;
    if (!ioctl_ok(s_encoder_fd, VIDIOC_REQBUFS, &encoder_input_request, "H.264 input VIDIOC_REQBUFS")) {
        return fail_video_initialization();
    }

    if (!set_pix_format(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, s_width, s_height, V4L2_PIX_FMT_H264) ||
        !map_buffers(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, s_encoder_buffers, kEncoderBufferCount,
                     &s_encoder_buffer_count) ||
        !queue_mmap_buffers(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, s_encoder_buffer_count) ||
        !stream_on(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "H.264 capture VIDIOC_STREAMON") ||
        !stream_on(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT, "H.264 output VIDIOC_STREAMON")) {
        return fail_video_initialization();
    }

    if (s_video_source == VideoSource::UsbUvc) {
        if (!prepare_usb_source_buffers()) {
            return fail_video_initialization();
        }
    }
    s_encoder_frame_sequence = 0;

    s_pipeline_ready = true;
    s_diag_pipeline_ready.store(true, std::memory_order_release);
    set_pipeline_state(PipelineState::Running);
    ESP_LOGI(kTag, "Camera: %" PRIu32 "x%" PRIu32 " %dfps H264 profile=baseline, bitrate=%d GOP=%d",
             s_width, s_height, CONFIG_TAB5_RTSP_FPS, CONFIG_TAB5_RTSP_BITRATE, CONFIG_TAB5_RTSP_GOP);
    return true;
}

static void shutdown_video(bool preserve_usb_source_buffers)
{
    if (s_pending_idr != nullptr) {
        heap_caps_free(s_pending_idr);
        s_pending_idr = nullptr;
        s_pending_idr_size = 0;
    }
    // Parameter sets belong to the current encoder instance. Never advertise
    // stale SPS/PPS after reconnect or bounded pipeline recovery.
    s_sps_size = 0;
    s_pps_size = 0;
    stream_off(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_OUTPUT);
    stream_off(s_encoder_fd, V4L2_BUF_TYPE_VIDEO_CAPTURE);
    unmap_buffers(s_encoder_buffers, s_encoder_buffer_count);
    if (s_encoder_fd >= 0) {
        close(s_encoder_fd);
    }
    s_encoder_fd = -1;
    s_encoder_buffer_count = 0;
    s_pipeline_ready = false;
    s_diag_pipeline_ready.store(false, std::memory_order_release);
    s_encoder_frame_sequence = 0;
    s_encode_debug_count = 0;
    s_rtp_access_unit_debug_count = 0;
    set_pipeline_state(PipelineState::Idle);
    // RTSP can disconnect before DESCRIBE/PLAY completes.  In that case this
    // client never acquired the shared MIPI capture and must not release the
    // preview's reference-counted capture session.
    if (!preserve_usb_source_buffers) {
        free_usb_source_buffers();
    }
    s_video_source = VideoSource::Mipi;
    s_diag_video_source.store(static_cast<uint8_t>(s_video_source), std::memory_order_release);
    s_width = kMipiWidth;
    s_height = kMipiHeight;
}

static EncodeResult encode_frame(const uint8_t** encoded, size_t* encoded_size, uint32_t* encoded_index)
{
    if (s_video_source == VideoSource::UsbUvc) {
        hal_uvc_frame_info_t info = {};
        bool converted = false;
        for (int attempt = 0; attempt < kUsbConversionRetryCount; ++attempt) {
            if (!copy_usb_frame(&info)) {
                ESP_LOGE(kTag, "USB UVC frame unavailable for H.264 after sequence=%" PRIu32, s_usb_sequence);
                return EncodeResult::Failed;
            }
            converted = info.format == HAL_UVC_FRAME_MJPEG ? decode_usb_mjpeg_to_yuv420(info) :
                        info.format == HAL_UVC_FRAME_YUY2 ? convert_usb_yuy2_to_yuv420(info) : false;
            if (converted) {
                break;
            }
            // Advance past the malformed frame so the next attempt asks the
            // UVC broker for a newer payload instead of decoding the same bytes.
            s_usb_sequence = info.sequence;
            if (attempt + 1 < kUsbConversionRetryCount) {
                vTaskDelay(pdMS_TO_TICKS(2));
            }
        }
        if (!converted) {
            ++s_usb_conversion_errors;
            ESP_LOGW(kTag, "USB UVC transient frame conversion failure; dropping frame attempts=%d/%d "
                     "format=%u size=%" PRIu32 "x%" PRIu32 " bytes=%u errors=%u",
                     kUsbConversionRetryCount, kUsbConversionRetryCount,
                     static_cast<unsigned>(info.format), info.width, info.height,
                     static_cast<unsigned>(info.bytes), static_cast<unsigned>(s_usb_conversion_errors));
            return EncodeResult::Dropped;
        }

        struct v4l2_buffer encoder_input = {};
        encoder_input.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        encoder_input.memory = V4L2_MEMORY_USERPTR;
        encoder_input.index = 0;
        encoder_input.m.userptr = reinterpret_cast<unsigned long>(s_usb_yuv420);
        encoder_input.length = kUsbYuv420Bytes;
        if (!ioctl_ok(s_encoder_fd, VIDIOC_QBUF, &encoder_input, "USB H.264 input VIDIOC_QBUF")) {
            record_encoder_error();
            return EncodeResult::Failed;
        }

        struct v4l2_buffer encoded_buffer = {};
        encoded_buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        encoded_buffer.memory = V4L2_MEMORY_MMAP;
        const int64_t output_deadline = esp_timer_get_time() + 1000000;
        while (ioctl(s_encoder_fd, VIDIOC_DQBUF, &encoded_buffer) != 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(kTag, "USB H.264 capture VIDIOC_DQBUF failed: errno=%d", errno);
                quiesce_encoder_dma();
                record_encoder_error();
                return EncodeResult::Failed;
            }
            const int64_t remaining = output_deadline - esp_timer_get_time();
            if (remaining <= 0) {
                ESP_LOGW(kTag, "USB H.264 output DQBUF deadline exceeded");
                quiesce_encoder_dma();
                record_encoder_timeout();
                return EncodeResult::Failed;
            }
            wait_fd_ready(s_encoder_fd, false, static_cast<int>(std::min<int64_t>(remaining / 1000, 50)));
        }
        if (encoded_buffer.bytesused == 0 || encoded_buffer.index >= s_encoder_buffer_count) {
            ESP_LOGW(kTag, "USB H.264 output is empty or buffer index is invalid");
            quiesce_encoder_dma();
            record_encoder_error();
            return EncodeResult::Failed;
        }

        struct v4l2_buffer completed_input = {};
        completed_input.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        completed_input.memory = V4L2_MEMORY_USERPTR;
        completed_input.index = 0;
        const int64_t input_deadline = esp_timer_get_time() + 1000000;
        while (ioctl(s_encoder_fd, VIDIOC_DQBUF, &completed_input) != 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                ESP_LOGE(kTag, "USB H.264 input VIDIOC_DQBUF failed: errno=%d", errno);
                quiesce_encoder_dma();
                record_encoder_error();
                return EncodeResult::Failed;
            }
            if (esp_timer_get_time() >= input_deadline) {
                ESP_LOGW(kTag, "USB H.264 input DQBUF deadline exceeded");
                quiesce_encoder_dma();
                record_encoder_timeout();
                return EncodeResult::Failed;
            }
            vTaskDelay(pdMS_TO_TICKS(1));
        }
        s_usb_sequence = info.sequence;
        *encoded = s_encoder_buffers[encoded_buffer.index].data;
        *encoded_size = encoded_buffer.bytesused;
        *encoded_index = encoded_buffer.index;
        if (*encoded == nullptr || *encoded_size == 0) {
            record_encoder_error();
            return EncodeResult::Failed;
        }
        return EncodeResult::Ready;
    }

    return EncodeResult::Failed;
}

static bool requeue_encoded(uint32_t index)
{
    struct v4l2_buffer buffer = {};
    buffer.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buffer.memory = V4L2_MEMORY_MMAP;
    buffer.index = index;
    return ioctl_ok(s_encoder_fd, VIDIOC_QBUF, &buffer, "H.264 capture requeue");
}

static size_t start_code_size(const uint8_t* data, size_t size, size_t offset)
{
    if (offset + 4 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 0 &&
        data[offset + 3] == 1) {
        return 4;
    }
    if (offset + 3 <= size && data[offset] == 0 && data[offset + 1] == 0 && data[offset + 2] == 1) {
        return 3;
    }
    return 0;
}

static bool cache_parameter_sets(const uint8_t* data, size_t size)
{
    size_t cursor = 0;
    bool found_sps = s_sps_size > 0;
    bool found_pps = s_pps_size > 0;
    while (cursor < size) {
        while (cursor < size && start_code_size(data, size, cursor) == 0) {
            ++cursor;
        }
        if (cursor >= size) {
            break;
        }
        const size_t prefix = start_code_size(data, size, cursor);
        const size_t nal_start = cursor + prefix;
        size_t next = nal_start;
        while (next < size && start_code_size(data, size, next) == 0) {
            ++next;
        }
        size_t nal_end = next;
        while (nal_end > nal_start && data[nal_end - 1] == 0) {
            --nal_end;
        }
        if (nal_end > nal_start) {
            const uint8_t nal_type = data[nal_start] & 0x1f;
            const size_t nal_size = nal_end - nal_start;
            if (nal_type == 7 && nal_size <= sizeof(s_sps)) {
                memcpy(s_sps, data + nal_start, nal_size);
                s_sps_size = nal_size;
                found_sps = true;
            } else if (nal_type == 8 && nal_size <= sizeof(s_pps)) {
                memcpy(s_pps, data + nal_start, nal_size);
                s_pps_size = nal_size;
                found_pps = true;
            }
        }
        cursor = next;
    }
    return found_sps && found_pps;
}

static bool access_unit_has_nal_type(const uint8_t *data, size_t size, uint8_t wanted_type)
{
    size_t cursor = 0;
    while (cursor < size) {
        while (cursor < size && start_code_size(data, size, cursor) == 0) {
            ++cursor;
        }
        if (cursor >= size) {
            break;
        }
        const size_t nal_start = cursor + start_code_size(data, size, cursor);
        if (nal_start < size && (data[nal_start] & 0x1f) == wanted_type) {
            return true;
        }
        cursor = nal_start;
    }
    return false;
}

static size_t base64_encode(const uint8_t* input, size_t input_size, char* output, size_t output_size)
{
    static constexpr char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t out = 0;
    for (size_t i = 0; i < input_size; i += 3) {
        const uint32_t value = (static_cast<uint32_t>(input[i]) << 16) |
                               ((i + 1 < input_size ? input[i + 1] : 0) << 8) |
                               (i + 2 < input_size ? input[i + 2] : 0);
        if (out + 4 >= output_size) {
            return 0;
        }
        output[out++] = alphabet[(value >> 18) & 0x3f];
        output[out++] = alphabet[(value >> 12) & 0x3f];
        output[out++] = i + 1 < input_size ? alphabet[(value >> 6) & 0x3f] : '=';
        output[out++] = i + 2 < input_size ? alphabet[value & 0x3f] : '=';
    }
    output[out] = '\0';
    return out;
}

static bool send_all(int fd, const void* data, size_t size, int64_t deadline = 0)
{
    const uint8_t* bytes = static_cast<const uint8_t*>(data);
    if (deadline == 0) {
        deadline = esp_timer_get_time() + static_cast<int64_t>(kSendDeadlineMs) * 1000;
    }
    while (size > 0) {
        const int sent = send(fd, bytes, size, MSG_DONTWAIT);
        if (sent > 0) {
            bytes += sent;
            size -= static_cast<size_t>(sent);
            s_bytes_sent += static_cast<size_t>(sent);
            s_lifetime_bytes.fetch_add(static_cast<uint64_t>(sent), std::memory_order_relaxed);
            continue;
        }
        if (sent == 0) {
            record_socket_error();
            ESP_LOGW(kTag, "RTSP send failed: send returned 0 errors=%u",
                     static_cast<unsigned>(s_send_socket_errors));
            return false;
        }
        if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
            record_socket_error();
            ESP_LOGW(kTag, "RTSP send failed: errno=%d errors=%u", errno,
                     static_cast<unsigned>(s_send_socket_errors));
            return false;
        }
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) {
            record_send_timeout();
            ESP_LOGW(kTag, "RTSP send deadline exceeded bytes_pending=%u timeouts=%u",
                     static_cast<unsigned>(size), static_cast<unsigned>(s_send_timeouts));
            return false;
        }
        wait_fd_ready(fd, true, static_cast<int>(std::min<int64_t>(remaining / 1000, 50)));
    }
    return true;
}

static bool send_rtsp_response(int fd, int cseq, const char* headers, const char* body = nullptr)
{
    char response[2048];
    const size_t body_size = body ? strlen(body) : 0;
    const int length = snprintf(response, sizeof(response),
                                "RTSP/1.0 200 OK\r\nCSeq: %d\r\n%sContent-Length: %zu\r\n\r\n%s",
                                cseq, headers ? headers : "", body_size, body ? body : "");
    return length > 0 && static_cast<size_t>(length) < sizeof(response) && send_all(fd, response, length);
}

static bool local_address(int fd, char *address, size_t capacity)
{
    sockaddr_in local{};
    socklen_t length = sizeof(local);
    return getsockname(fd, reinterpret_cast<sockaddr *>(&local), &length) == 0 &&
           inet_ntop(AF_INET, &local.sin_addr, address, capacity) != nullptr;
}

static bool send_sdp(int fd, int cseq)
{
    char sps_b64[384] = {};
    char pps_b64[192] = {};
    base64_encode(s_sps, s_sps_size, sps_b64, sizeof(sps_b64));
    base64_encode(s_pps, s_pps_size, pps_b64, sizeof(pps_b64));

    char profile_level_id[] = "42e01f";
    if (s_sps_size >= 4) {
        snprintf(profile_level_id, sizeof(profile_level_id), "%02x%02x%02x", s_sps[1], s_sps[2], s_sps[3]);
    }

    char sdp[1200];
    if (s_sps_size > 0 && s_pps_size > 0) {
        snprintf(sdp, sizeof(sdp),
                 "v=0\r\n"
                 "o=- 0 0 IN IP4 0.0.0.0\r\n"
                 "s=Tab5 RTSP Camera\r\n"
                 "t=0 0\r\n"
                 "a=control:*\r\n"
                 "m=video 0 RTP/AVP 96\r\n"
                 "c=IN IP4 0.0.0.0\r\n"
                 "a=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 packetization-mode=1;profile-level-id=%s;sprop-parameter-sets=%s,%s\r\n"
                 "a=framerate:%d\r\n"
                 "a=control:trackID=0\r\n",
                 profile_level_id, sps_b64, pps_b64, CONFIG_TAB5_RTSP_FPS);
    } else {
        snprintf(sdp, sizeof(sdp),
                 "v=0\r\n"
                 "o=- 0 0 IN IP4 0.0.0.0\r\n"
                 "s=Tab5 RTSP Camera\r\n"
                 "t=0 0\r\n"
                 "a=control:*\r\n"
                 "m=video 0 RTP/AVP 96\r\n"
                 "c=IN IP4 0.0.0.0\r\n"
                 "a=rtpmap:96 H264/90000\r\n"
                 "a=fmtp:96 packetization-mode=1;profile-level-id=%s\r\n"
                 "a=framerate:%d\r\n"
                 "a=control:trackID=0\r\n",
                 profile_level_id, CONFIG_TAB5_RTSP_FPS);
    }

    char address[INET_ADDRSTRLEN];
    if (!local_address(fd, address, sizeof(address))) return false;
    char headers[256];
    snprintf(headers, sizeof(headers), "Content-Base: rtsp://%s:%u/baby/\r\nContent-Type: application/sdp\r\n",
             address, kRtspPort);
    return send_rtsp_response(fd, cseq, headers, sdp);
}

static bool send_play_response(int fd, int cseq)
{
    // RTP-Info identifies the first RTP packet that follows PLAY.  FFmpeg
    // stream-copy can otherwise start with an unset RTP-to-media timestamp
    // mapping, which produces unset or non-monotonic DTS in a rebroadcast.
    char headers[320];
    char address[INET_ADDRSTRLEN];
    if (!local_address(fd, address, sizeof(address))) return false;
    snprintf(headers, sizeof(headers),
             "Session: 12345678\r\n"
             "Range: npt=0.000-\r\n"
             "RTP-Info: url=rtsp://%s:%u/baby/trackID=0;seq=%u;rtptime=%" PRIu32 "\r\n",
             address, static_cast<unsigned>(kRtspPort), static_cast<unsigned>(s_rtp_sequence), s_rtp_timestamp);
    return send_rtsp_response(fd, cseq, headers);
}

static bool send_rtp_packet(int fd, const uint8_t* payload, size_t payload_size, uint32_t timestamp,
                            bool marker, int64_t deadline)
{
    // Send the interleaved header and RTP packet as one contiguous wire frame.
    // This reduces small TCP writes and prevents a peer from observing a
    // header without its RTP payload when the socket becomes backpressured.
    uint8_t wire[4 + 12 + kMaxRtpPayload];
    const size_t rtp_size = 12 + payload_size;
    wire[0] = '$';
    wire[1] = s_rtp_channel;
    wire[2] = static_cast<uint8_t>(rtp_size >> 8);
    wire[3] = static_cast<uint8_t>(rtp_size);
    wire[4] = 0x80;
    wire[5] = static_cast<uint8_t>(kRtpPayloadType | (marker ? 0x80 : 0));
    wire[6] = static_cast<uint8_t>(s_rtp_sequence >> 8);
    wire[7] = static_cast<uint8_t>(s_rtp_sequence++);
    wire[8] = static_cast<uint8_t>(timestamp >> 24);
    wire[9] = static_cast<uint8_t>(timestamp >> 16);
    wire[10] = static_cast<uint8_t>(timestamp >> 8);
    wire[11] = static_cast<uint8_t>(timestamp);
    wire[12] = static_cast<uint8_t>(kRtpSsrc >> 24);
    wire[13] = static_cast<uint8_t>(kRtpSsrc >> 16);
    wire[14] = static_cast<uint8_t>(kRtpSsrc >> 8);
    wire[15] = static_cast<uint8_t>(kRtpSsrc);
    memcpy(wire + 16, payload, payload_size);
    const bool sent = send_all(fd, wire, sizeof(wire[0]) * (4 + rtp_size), deadline);
    if (sent) {
        s_lifetime_packets.fetch_add(1, std::memory_order_relaxed);
        s_last_rtp_packet_us = esp_timer_get_time();
    }
    return sent;
}

static bool send_h264_nal(int fd, const uint8_t* nal, size_t nal_size, uint32_t timestamp, bool marker,
                          int64_t deadline)
{
    if (nal_size <= kMaxRtpPayload) {
        return send_rtp_packet(fd, nal, nal_size, timestamp, marker, deadline);
    }

    const uint8_t nal_header = nal[0];
    const uint8_t fu_indicator = static_cast<uint8_t>((nal_header & 0xe0) | 28);
    const uint8_t nal_type = nal_header & 0x1f;
    size_t offset = 1;
    while (offset < nal_size) {
        const size_t chunk = (nal_size - offset > kMaxRtpPayload - 2) ? kMaxRtpPayload - 2 : nal_size - offset;
        uint8_t fragment[kMaxRtpPayload];
        fragment[0] = fu_indicator;
        fragment[1] = static_cast<uint8_t>(nal_type | (offset == 1 ? 0x80 : 0) |
                                            (offset + chunk == nal_size ? 0x40 : 0));
        memcpy(fragment + 2, nal + offset, chunk);
        const bool last = offset + chunk == nal_size;
        if (!send_rtp_packet(fd, fragment, chunk + 2, timestamp, last && marker, deadline)) {
            return false;
        }
        offset += chunk;
    }
    return true;
}

static bool send_h264_access_unit(int fd, const uint8_t* data, size_t size, uint32_t timestamp)
{
    // A slow receiver gets one deadline for the whole access unit, not a fresh
    // second for every FU-A fragment.
    const int64_t deadline = esp_timer_get_time() + static_cast<int64_t>(kSendDeadlineMs) * 1000;
    size_t cursor = 0;
    size_t nal_count = 0;
    uint32_t nal_type_mask = 0;
    while (cursor < size) {
        while (cursor < size && start_code_size(data, size, cursor) == 0) {
            ++cursor;
        }
        if (cursor >= size) {
            break;
        }
        const size_t nal_start = cursor + start_code_size(data, size, cursor);
        size_t next = nal_start;
        while (next < size && start_code_size(data, size, next) == 0) {
            ++next;
        }
        size_t nal_end = next;
        while (nal_end > nal_start && data[nal_end - 1] == 0) {
            --nal_end;
        }
        if (nal_end > nal_start) {
            ++nal_count;
            nal_type_mask |= 1U << (data[nal_start] & 0x1f);
        }
        cursor = next;
    }
    if (nal_count == 0) {
        return false;
    }
    const uint32_t debug_index = s_rtp_access_unit_debug_count++;
    if (debug_index < 4) {
        ESP_LOGI(kTag, "H.264 RTP access unit[%" PRIu32 "]: bytes=%u NALs=%u type_mask=0x%08" PRIx32,
                 debug_index, static_cast<unsigned int>(size), static_cast<unsigned int>(nal_count), nal_type_mask);
    }

    cursor = 0;
    size_t current = 0;
    while (cursor < size) {
        while (cursor < size && start_code_size(data, size, cursor) == 0) {
            ++cursor;
        }
        if (cursor >= size) {
            break;
        }
        const size_t nal_start = cursor + start_code_size(data, size, cursor);
        size_t next = nal_start;
        while (next < size && start_code_size(data, size, next) == 0) {
            ++next;
        }
        size_t nal_end = next;
        while (nal_end > nal_start && data[nal_end - 1] == 0) {
            --nal_end;
        }
        if (nal_end > nal_start && !send_h264_nal(fd, data + nal_start, nal_end - nal_start, timestamp,
                                                   ++current == nal_count, deadline)) {
            return false;
        }
        cursor = next;
    }
    s_lifetime_access_units.fetch_add(1, std::memory_order_relaxed);
    return true;
}

static bool prime_parameter_sets(void)
{
    if (s_pending_idr != nullptr) {
        heap_caps_free(s_pending_idr);
        s_pending_idr = nullptr;
        s_pending_idr_size = 0;
    }
    for (int attempt = 0; attempt < 5; ++attempt) {
        ESP_LOGI(kTag, "H.264 prime frame attempt %d", attempt + 1);
        const uint8_t* encoded = nullptr;
        size_t encoded_size = 0;
        uint32_t encoded_index = 0;
        const EncodeResult encode_result = encode_frame(&encoded, &encoded_size, &encoded_index);
        if (encode_result == EncodeResult::Dropped) {
            ESP_LOGW(kTag, "H.264 prime frame dropped on attempt %d", attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(2));
            continue;
        }
        if (encode_result == EncodeResult::Failed) {
            ESP_LOGE(kTag, "H.264 prime frame encode failed on attempt %d", attempt + 1);
            return false;
        }
        ESP_LOGI(kTag, "H.264 prime frame encoded: size=%u buffer=%u", static_cast<unsigned int>(encoded_size),
                 static_cast<unsigned int>(encoded_index));
        const bool has_parameter_sets = cache_parameter_sets(encoded, encoded_size);
        const bool has_idr = access_unit_has_nal_type(encoded, encoded_size, 5);
        if (has_parameter_sets && has_idr) {
            s_pending_idr = static_cast<uint8_t*>(heap_caps_malloc(encoded_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
            if (s_pending_idr == nullptr) {
                ESP_LOGE(kTag, "failed to preserve initial H.264 IDR: %u bytes",
                         static_cast<unsigned int>(encoded_size));
                if (!requeue_encoded(encoded_index)) {
                    record_encoder_error();
                }
                return false;
            }
            memcpy(s_pending_idr, encoded, encoded_size);
            s_pending_idr_size = encoded_size;
        }
        if (!requeue_encoded(encoded_index)) {
            record_encoder_error();
            if (s_pending_idr != nullptr) {
                heap_caps_free(s_pending_idr);
                s_pending_idr = nullptr;
                s_pending_idr_size = 0;
            }
            return false;
        }
        if (has_parameter_sets && has_idr) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
    ESP_LOGE(kTag, "H.264 stream did not provide SPS/PPS");
    return false;
}

static int request_cseq(const char* request)
{
    const char* cseq = strstr(request, "CSeq:");
    return cseq ? atoi(cseq + 5) : 1;
}

static FrameResult stream_one_frame(int client_fd)
{
    if (s_pending_idr != nullptr && s_pending_idr_size > 0) {
        ESP_LOGI(kTag, "sending preserved initial H.264 IDR: bytes=%u",
                 static_cast<unsigned int>(s_pending_idr_size));
        const bool sent = send_h264_access_unit(client_fd, s_pending_idr, s_pending_idr_size, s_rtp_timestamp);
        s_rtp_timestamp += kRtpClockRate / CONFIG_TAB5_RTSP_FPS;
        heap_caps_free(s_pending_idr);
        s_pending_idr = nullptr;
        s_pending_idr_size = 0;
        return sent ? FrameResult::Sent : FrameResult::Failed;
    }
    const uint8_t* encoded = nullptr;
    size_t encoded_size = 0;
    uint32_t encoded_index = 0;
    const EncodeResult encode_result = encode_frame(&encoded, &encoded_size, &encoded_index);
    if (encode_result == EncodeResult::Dropped) {
        return FrameResult::Dropped;
    }
    if (encode_result == EncodeResult::Failed) {
        return FrameResult::Failed;
    }
    cache_parameter_sets(encoded, encoded_size);
    const bool sent = send_h264_access_unit(client_fd, encoded, encoded_size, s_rtp_timestamp);
    s_rtp_timestamp += kRtpClockRate / CONFIG_TAB5_RTSP_FPS;
    const bool requeued = requeue_encoded(encoded_index);
    if (!requeued) {
        record_encoder_error();
    }
    return sent && requeued ? FrameResult::Sent : FrameResult::Failed;
}

static void handle_client(int client_fd)
{
    bool setup = false;
    bool playing = false;
    int recovery_attempts = 0;
    int consecutive_frame_drops = 0;
    int64_t next_frame_us = 0;
    s_last_rtp_packet_us = 0;
    tab5::rtsp::InputFramer framer;
    uint8_t receive_buffer[512] = {};

    while (true) {
        const size_t writable = framer.writable_capacity();
        if (writable > 0) {
            const size_t receive_size = std::min(sizeof(receive_buffer), writable);
            const int received = recv(client_fd, receive_buffer, receive_size, MSG_DONTWAIT);
            if (received == 0) {
                break;
            }
            if (received > 0) {
                if (!framer.append(receive_buffer, static_cast<size_t>(received))) {
                    ESP_LOGW(kTag, "RTSP input framing buffer overflow");
                    break;
                }
            } else if (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) {
                break;
            }
        }

        const tab5::rtsp::InputFrame input = framer.next();
        if (input.type == tab5::rtsp::InputFrameType::Error) {
            ESP_LOGW(kTag, "RTSP input framing rejected: request or interleaved payload too large");
            break;
        }
        if (input.type == tab5::rtsp::InputFrameType::NeedMore) {
            if (framer.writable_capacity() == 0) {
                ESP_LOGW(kTag, "RTSP input framing buffer exhausted before a complete frame");
                break;
            }
            // Do not continue here. RTSP/TCP clients normally have no text
            // request to read while PLAY is active, so the frame scheduler
            // below must also run when recv() produced no complete input.
            vTaskDelay(pdMS_TO_TICKS(1));
        } else if (input.type == tab5::rtsp::InputFrameType::Interleaved) {
            // The parser has isolated binary RTCP/RTP data before any text
            // search. Embedded zero bytes therefore cannot corrupt RTSP input.
            if (input.channel == s_rtcp_channel) {
                ++s_rtcp_packets;
            }
            framer.consume(input.wire_size);
        } else {
            char request[kMaxRtspRequest + 1] = {};
            memcpy(request, input.data, input.size);
            request[input.size] = '\0';
            char method[16] = {};
            char uri[256] = {};
            sscanf(request, "%15s %255s", method, uri);
            const int cseq = request_cseq(request);

            ESP_LOGI(kTag, "RTSP request: %s CSeq=%d", method, cseq);

            if (strcmp(method, "OPTIONS") == 0) {
                if (!send_rtsp_response(client_fd, cseq, "Public: OPTIONS, DESCRIBE, SETUP, PLAY, GET_PARAMETER, TEARDOWN\r\n")) {
                    break;
                }
            } else if (strcmp(method, "DESCRIBE") == 0) {
                const int64_t describe_start_us = esp_timer_get_time();
                ESP_LOGI(kTag, "RTSP DESCRIBE begin CSeq=%d", cseq);

                const int64_t initialize_start_us = esp_timer_get_time();
                ESP_LOGI(kTag, "RTSP DESCRIBE initialize_video begin");
                const bool initialized = initialize_video();
                ESP_LOGI(kTag, "RTSP DESCRIBE initialize_video result=%s elapsed_ms=%" PRId64
                         " source=%s size=%" PRIu32 "x%" PRIu32,
                         initialized ? "ok" : "failed",
                         (esp_timer_get_time() - initialize_start_us) / 1000,
                         s_video_source == VideoSource::UsbUvc ? "USB UVC" : "internal MIPI", s_width, s_height);

                bool primed = false;
                if (initialized) {
                    const int64_t prime_start_us = esp_timer_get_time();
                    ESP_LOGI(kTag, "RTSP DESCRIBE prime_parameter_sets begin");
                    primed = prime_parameter_sets();
                    ESP_LOGI(kTag, "RTSP DESCRIBE prime_parameter_sets result=%s elapsed_ms=%" PRId64
                             " sps=%u pps=%u pending_idr=%u",
                             primed ? "ok" : "failed",
                             (esp_timer_get_time() - prime_start_us) / 1000,
                             static_cast<unsigned int>(s_sps_size), static_cast<unsigned int>(s_pps_size),
                             static_cast<unsigned int>(s_pending_idr_size));
                }

                bool sdp_sent = false;
                if (initialized && primed) {
                    const int64_t sdp_start_us = esp_timer_get_time();
                    ESP_LOGI(kTag, "RTSP DESCRIBE send_sdp begin");
                    sdp_sent = send_sdp(client_fd, cseq);
                    ESP_LOGI(kTag, "RTSP DESCRIBE send_sdp result=%s elapsed_ms=%" PRId64,
                             sdp_sent ? "ok" : "failed", (esp_timer_get_time() - sdp_start_us) / 1000);
                }

                const bool described = initialized && primed && sdp_sent;
                ESP_LOGI(kTag, "RTSP DESCRIBE end result=%s total_ms=%" PRId64,
                         described ? "ok" : "failed", (esp_timer_get_time() - describe_start_us) / 1000);
                // Keep this connection's initialized pipeline and primed IDR
                // for SETUP/PLAY. Closing and reopening between DESCRIBE and
                // PLAY fragments the small internal-RAM heap used by the
                // hardware encoder. The connection cleanup below still stops
                // DESCRIBE-only probes when the client disconnects.
                if (!described) {
                    shutdown_video();
                    break;
                }
            } else if (strcmp(method, "SETUP") == 0) {
                if (!tab5::rtsp::request_has_tcp_transport(input.data, input.size)) {
                    const char* response = "RTSP/1.0 461 Unsupported Transport\r\n";
                    char full[128];
                    snprintf(full, sizeof(full), "%sCSeq: %d\r\n\r\n", response, cseq);
                    if (!send_all(client_fd, full, strlen(full))) {
                        break;
                    }
                } else {
                    uint8_t requested_rtp = 0;
                    uint8_t requested_rtcp = 1;
                    if (!tab5::rtsp::parse_interleaved_channels(input.data, input.size, &requested_rtp,
                                                                &requested_rtcp)) {
                        const char* response = "RTSP/1.0 461 Unsupported Transport\r\n";
                        char full[128];
                        snprintf(full, sizeof(full), "%sCSeq: %d\r\n\r\n", response, cseq);
                        if (!send_all(client_fd, full, strlen(full))) {
                            break;
                        }
                    } else {
                        s_rtp_channel = requested_rtp;
                        s_rtcp_channel = requested_rtcp;
                        setup = true;
                        char transport_header[192];
                        snprintf(transport_header, sizeof(transport_header),
                                 "Transport: RTP/AVP/TCP;unicast;interleaved=%u-%u\r\nSession: 12345678\r\n",
                                 static_cast<unsigned>(s_rtp_channel), static_cast<unsigned>(s_rtcp_channel));
                        if (!send_rtsp_response(client_fd, cseq, transport_header)) {
                            break;
                        }
                    }
                }
            } else if (strcmp(method, "PLAY") == 0) {
                const bool starting_playback = !playing;
                if (!setup || !initialize_video()) {
                    break;
                }
                if (starting_playback) {
                    if (s_pending_idr == nullptr && !prime_parameter_sets()) {
                        break;
                    }
                    playing = true;
                    recovery_attempts = 0;
                    s_last_rtp_packet_us = 0;
                    s_rtp_sequence = static_cast<uint16_t>(esp_timer_get_time());
                    s_rtp_timestamp = static_cast<uint32_t>(esp_timer_get_time() * 90);
                    next_frame_us = esp_timer_get_time();
                }
                if (!send_play_response(client_fd, cseq)) {
                    break;
                }
                // Arm the watchdog even if the first encode/send attempt
                // never produces an RTP packet.
                if (starting_playback) {
                    s_last_rtp_packet_us = esp_timer_get_time();
                }
            } else if (strcmp(method, "GET_PARAMETER") == 0) {
                if (!send_rtsp_response(client_fd, cseq, "Session: 12345678\r\n")) {
                    break;
                }
            } else if (strcmp(method, "TEARDOWN") == 0) {
                send_rtsp_response(client_fd, cseq, "Session: 12345678\r\n");
                break;
            }

            framer.consume(input.wire_size);
        }

        if (playing && setup) {
            const int64_t now = esp_timer_get_time();
            if (s_last_rtp_packet_us != 0 && now - s_last_rtp_packet_us >= kRtpPacketStallTimeoutUs) {
                s_lifetime_rtp_stall_watchdogs.fetch_add(1, std::memory_order_relaxed);
                ESP_LOGW(kTag, "RTSP RTP packet watchdog expired; restarting stalled pipeline");
                if (recovery_attempts == 0) {
                    ++recovery_attempts;
                    s_lifetime_recoveries.fetch_add(1, std::memory_order_relaxed);
                    set_pipeline_state(PipelineState::Recovering);
                    shutdown_video(true);
                    s_last_rtp_packet_us = 0;
                    consecutive_frame_drops = 0;
                    if (initialize_video() && prime_parameter_sets()) {
                        next_frame_us = esp_timer_get_time();
                        continue;
                    }
                }
                ESP_LOGE(kTag, "RTSP RTP packet watchdog recovery failed; disconnecting client");
                break;
            }
            if (now >= next_frame_us) {
                const uint32_t send_timeouts_before = s_send_timeouts;
                const uint32_t send_socket_errors_before = s_send_socket_errors;
                const FrameResult frame_result = stream_one_frame(client_fd);
                if (frame_result == FrameResult::Dropped) {
                    ++consecutive_frame_drops;
                    if (consecutive_frame_drops < kMaxConsecutiveFrameDrops) {
                        next_frame_us = now + 1000000LL / CONFIG_TAB5_RTSP_FPS;
                        continue;
                    }
                    ESP_LOGW(kTag, "too many consecutive UVC frame drops; treating as runtime failure count=%d",
                             consecutive_frame_drops);
                } else {
                    consecutive_frame_drops = 0;
                }
                if (frame_result == FrameResult::Failed ||
                    consecutive_frame_drops >= kMaxConsecutiveFrameDrops) {
                    if (s_send_timeouts > send_timeouts_before ||
                        s_send_socket_errors > send_socket_errors_before) {
                        ESP_LOGW(kTag, "RTSP peer send failure; disconnecting without encoder recovery");
                        break;
                    }
                    if (recovery_attempts == 0) {
                        ++recovery_attempts;
                        s_lifetime_recoveries.fetch_add(1, std::memory_order_relaxed);
                        set_pipeline_state(PipelineState::Recovering);
                        ESP_LOGW(kTag, "H.264 runtime failure; bounded recovery attempt");
                        shutdown_video(true);
                        if (initialize_video() && prime_parameter_sets()) {
                            next_frame_us = esp_timer_get_time();
                            continue;
                        }
                    }
                    ESP_LOGE(kTag, "H.264 runtime failure after recovery; disconnecting client");
                    break;
                }
                next_frame_us = now + 1000000LL / CONFIG_TAB5_RTSP_FPS;
            } else {
                vTaskDelay(pdMS_TO_TICKS(1));
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

static void rtsp_task(void*)
{
    while (true) {
        struct sockaddr_storage address = {};
        socklen_t address_length = sizeof(address);
        const int client_fd = accept(s_listen_fd, reinterpret_cast<struct sockaddr*>(&address), &address_length);
        if (client_fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        s_lifetime_connections.fetch_add(1, std::memory_order_relaxed);
        s_diag_active_client.store(true, std::memory_order_release);
        s_rtp_sequence = static_cast<uint16_t>(esp_timer_get_time());
        s_rtp_timestamp = static_cast<uint32_t>(esp_timer_get_time() * 90);
        s_rtp_channel = 0;
        s_rtcp_channel = 1;
        s_bytes_sent = 0;
        s_send_timeouts = 0;
        s_send_socket_errors = 0;
        s_encoder_timeouts = 0;
        s_encoder_errors = 0;
        s_usb_conversion_errors = 0;
        s_rtcp_packets = 0;
        s_last_rtp_packet_us = 0;
        ESP_LOGI(kTag, "RTSP client connected");
        handle_client(client_fd);
        shutdown_video();
        close(client_fd);
        s_diag_active_client.store(false, std::memory_order_release);
        ESP_LOGI(kTag, "RTSP client disconnected bytes=%llu send_timeouts=%u send_socket_errors=%u "
                 "encoder_timeouts=%u encoder_errors=%u usb_conversion_errors=%u rtcp=%u stack=%u",
                 static_cast<unsigned long long>(s_bytes_sent), static_cast<unsigned>(s_send_timeouts),
                 static_cast<unsigned>(s_send_socket_errors),
                 static_cast<unsigned>(s_encoder_timeouts), static_cast<unsigned>(s_encoder_errors),
                 static_cast<unsigned>(s_usb_conversion_errors),
                 static_cast<unsigned>(s_rtcp_packets),
                 static_cast<unsigned>(uxTaskGetStackHighWaterMark(nullptr)));
    }
}

} // namespace

extern "C" esp_err_t rtsp_server_start(void)
{
    if (s_server_started) {
        return ESP_OK;
    }

    s_listen_fd = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    if (s_listen_fd < 0) {
        ESP_LOGE(kTag, "RTSP socket creation failed: errno=%d", errno);
        return ESP_FAIL;
    }
    int reuse = 1;
    setsockopt(s_listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in address = {};
    address.sin_family = AF_INET;
    address.sin_port = htons(kRtspPort);
    address.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(s_listen_fd, reinterpret_cast<struct sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(s_listen_fd, 1) != 0) {
        ESP_LOGE(kTag, "RTSP bind/listen failed: errno=%d", errno);
        close(s_listen_fd);
        s_listen_fd = -1;
        return ESP_FAIL;
    }

    // The RTSP handler's measured stack usage is about 7 KiB, and runtime HWM
    // reached 72 bytes after a client disconnected. On ESP32-P4 StackType_t is
    // byte-sized, so reserve 20 KiB explicitly. Keep the task control block
    // internal, but move this long-lived stack to PSRAM so it cannot fragment
    // the internal H.264 reference-frame heap before a client opens the encoder.
    constexpr uint32_t kRtspTaskStackBytes = 20 * 1024;
    constexpr UBaseType_t kExternalStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    TaskHandle_t created_task = nullptr;
    const BaseType_t task_result = xTaskCreateWithCaps(rtsp_task, "rtsp", kRtspTaskStackBytes, nullptr, 5, &created_task,
                                                       kExternalStackCaps);
    if (task_result != pdPASS) {
        ESP_LOGE(kTag, "RTSP task creation failed");
        close(s_listen_fd);
        s_listen_fd = -1;
        s_rtsp_task.store(nullptr, std::memory_order_release);
        s_server_started = false;
        return ESP_ERR_NO_MEM;
    }
    s_rtsp_task.store(created_task, std::memory_order_release);
    s_server_started = true;
    s_diag_server_started.store(true, std::memory_order_release);
    ESP_LOGI(kTag, "RTSP server started: rtsp://%s.local:%u/baby", CONFIG_EDGE_HOSTNAME, kRtspPort);
    return ESP_OK;
}

extern "C" bool rtsp_server_get_metrics(rtsp_metrics_t *metrics)
{
    if (metrics == nullptr) {
        return false;
    }
    memset(metrics, 0, sizeof(*metrics));
    metrics->server_started = s_diag_server_started.load(std::memory_order_acquire);
    metrics->pipeline_ready = s_diag_pipeline_ready.load(std::memory_order_acquire);
    metrics->active_client = s_diag_active_client.load(std::memory_order_acquire);
    metrics->pipeline_state = s_diag_pipeline_state.load(std::memory_order_acquire);
    metrics->video_source = s_diag_video_source.load(std::memory_order_acquire);
    const TaskHandle_t rtsp_task_handle = s_rtsp_task.load(std::memory_order_acquire);
    metrics->task_stack_hwm = rtsp_task_handle != nullptr
                                  ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(rtsp_task_handle))
                                  : 0;
    metrics->connections = s_lifetime_connections.load(std::memory_order_relaxed);
    metrics->access_units = s_lifetime_access_units.load(std::memory_order_relaxed);
    metrics->packets = s_lifetime_packets.load(std::memory_order_relaxed);
    metrics->bytes = s_lifetime_bytes.load(std::memory_order_relaxed);
    metrics->send_timeouts = s_lifetime_send_timeouts.load(std::memory_order_relaxed);
    metrics->socket_errors = s_lifetime_socket_errors.load(std::memory_order_relaxed);
    metrics->encoder_timeouts = s_lifetime_encoder_timeouts.load(std::memory_order_relaxed);
    metrics->encoder_errors = s_lifetime_encoder_errors.load(std::memory_order_relaxed);
    metrics->recoveries = s_lifetime_recoveries.load(std::memory_order_relaxed);
    metrics->rtp_stall_watchdogs = s_lifetime_rtp_stall_watchdogs.load(std::memory_order_relaxed);
    return true;
}
