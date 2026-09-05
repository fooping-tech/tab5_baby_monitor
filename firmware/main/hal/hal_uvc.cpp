/*
 * SPDX-License-Identifier: MIT
 *
 * Native Espressif USB Host UVC client. The USB Host library is installed by
 * the Tab5 BSP; this component installs only uvc_host and keeps a small copy
 * broker between transfer callbacks and the LVGL renderer.
 */
#include "hal/hal_uvc.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <inttypes.h>
#include <mutex>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/uvc_host.h"

namespace {
constexpr char kTag[] = "uvc";
constexpr uint32_t kWidth = CONFIG_TAB5_UVC_WIDTH;
constexpr uint32_t kHeight = CONFIG_TAB5_UVC_HEIGHT;
constexpr size_t kMaxFrameBytes = 2 * 1024 * 1024;
constexpr size_t kSlotCount = 3;
// Some UVC cameras keep the stream handle alive after isochronous payload
// errors, but stop producing complete frames. Detect that state and reopen
// the same negotiated profile so the local preview can recover without a
// physical USB reconnect.
constexpr int64_t kFrameStallTimeoutUs = 2 * 1000 * 1000;
enum class State : uint8_t {
    Uninitialized, DriverReady, DeviceAbsent, DevicePresent,
    Negotiating, Streaming, Disconnected, Error,
};
struct Slot {
    uint8_t *data = nullptr;
    size_t bytes = 0;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    enum uvc_host_stream_format format = UVC_VS_FORMAT_DEFAULT;
    uint32_t sequence = 0;
    int64_t captured_us = 0;
    uint32_t generation = 0;
    bool ready = false;
    bool rendering = false;
    bool writing = false;
    bool copying = false;
    bool valid = false;
};

std::atomic<State> s_state{State::Uninitialized};
std::atomic<bool> s_initialized{false};
std::atomic<bool> s_start_requested{false};
std::atomic<bool> s_stop_requested{false};
std::atomic<bool> s_capturing{false};
std::atomic<bool> s_streaming{false};
// If a task did not acknowledge stop, its buffers/handle may still be live.
// Fail closed instead of allowing a second capture session to race it.
std::atomic<bool> s_restart_blocked{false};
std::atomic<bool> s_stop_pending{false};
std::atomic<bool> s_stream_stop_complete{true};
std::atomic<bool> s_render_stop_complete{true};
std::atomic<uint32_t> s_active_copies{0};
std::atomic<bool> s_frame_format_error{false};
std::atomic<bool> s_connected{false};
std::atomic<uint32_t> s_generation{0};
std::mutex s_mutex;
Slot s_slots[kSlotCount];
uint32_t s_sequence = 0;
std::atomic<uint32_t> s_frames{0};
std::atomic<uint32_t> s_drops{0};
std::atomic<uint32_t> s_callback_max_us{0};
std::atomic<uint32_t> s_callback_slow{0};
std::atomic<uint32_t> s_preview_frames{0};
std::atomic<uint32_t> s_preview_decode_errors{0};
std::atomic<uint32_t> s_watchdog_restarts{0};
std::atomic<int64_t> s_last_frame_us{0};
std::atomic<int64_t> s_last_preview_us{0};
std::atomic<uint8_t> s_device_address{0};
std::atomic<uint8_t> s_stream_index{0};
uvc_host_stream_format_t s_negotiated_format = {};
uvc_host_frame_info_t s_profiles[16] = {};
size_t s_profile_count = 0;
// The disconnect callback and stream task race to release one handle. Only a
// compare/exchange of that exact handle owns the stop/close operation.
std::atomic<uvc_host_stream_hdl_t> s_owned_stream{nullptr};
TaskHandle_t s_stream_task = nullptr;
TaskHandle_t s_render_task = nullptr;
SemaphoreHandle_t s_start_event = nullptr;
SemaphoreHandle_t s_stop_done = nullptr;
SemaphoreHandle_t s_stream_done = nullptr;
static void maybe_finish_stop()
{
    if (!s_stop_pending.load()) {
        return;
    }
    if (!s_stream_stop_complete.load() || !s_render_stop_complete.load() || s_active_copies.load() != 0) {
        return;
    }
    s_stop_pending.store(false);
}

static const char *format_name(enum uvc_host_stream_format format)
{
    switch (format) {
    case UVC_VS_FORMAT_MJPEG:
        return "MJPEG";
    case UVC_VS_FORMAT_YUY2:
        return "YUY2";
    default:
        return "unknown";
    }
}

static void clear_profiles_locked()
{
    s_profile_count = 0;
    memset(s_profiles, 0, sizeof(s_profiles));
}

static bool mark_stream_open(uvc_host_stream_hdl_t stream)
{
    uvc_host_stream_hdl_t expected = nullptr;
    return s_owned_stream.compare_exchange_strong(expected, stream, std::memory_order_acq_rel);
}

static bool stream_handle_is_owned()
{
    // A disconnect callback or an earlier stream task may still own a handle.
    // Do not call stream_open while it is live: the UVC host can otherwise
    // deliver callbacks to the old handle after the new session starts.
    return s_owned_stream.load(std::memory_order_acquire) != nullptr;
}

static void close_owned_stream(uvc_host_stream_hdl_t stream, bool stop_first)
{
    if (stream == nullptr) {
        return;
    }
    uvc_host_stream_hdl_t expected = stream;
    if (!s_owned_stream.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel)) {
        return;
    }
    if (stop_first) {
        uvc_host_stream_stop(stream);
    }
    uvc_host_stream_close(stream);
    // A previous ownership collision is recoverable once the old handle has
    // actually been closed.  Keep the block set while the handle is live so
    // a new stream cannot race callbacks from that stale session.
    if (s_owned_stream.load(std::memory_order_acquire) == nullptr) {
        s_restart_blocked.store(false, std::memory_order_release);
    }
}

static bool frame_callback(const uvc_host_frame_t *frame, void *)
{
    if (frame == nullptr || !s_streaming.load() || frame->data == nullptr || frame->data_len == 0) {
        return true;
    }
    if ((frame->vs_format.format != UVC_VS_FORMAT_MJPEG && frame->vs_format.format != UVC_VS_FORMAT_YUY2) ||
        frame->vs_format.h_res == 0 || frame->vs_format.v_res == 0) {
        s_streaming.store(false);
        s_frame_format_error.store(true, std::memory_order_release);
        s_state.store(State::Error);
        ESP_LOGE(kTag, "UVC frame format invalid: format=%u size=%" PRIu32 "x%" PRIu32,
                 static_cast<unsigned>(frame->vs_format.format),
                 static_cast<uint32_t>(frame->vs_format.h_res),
                 static_cast<uint32_t>(frame->vs_format.v_res));
        return true;
    }
    if (frame->data_len > kMaxFrameBytes) {
        s_drops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const size_t bytes = frame->data_len;
    const uint32_t callback_generation = s_generation.load();
    Slot *slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        uint32_t oldest = UINT32_MAX;
        for (auto &candidate : s_slots) {
            if (!candidate.rendering && !candidate.ready && !candidate.writing && !candidate.copying &&
                candidate.sequence < oldest) {
                oldest = candidate.sequence;
                slot = &candidate;
            }
        }
        if (slot != nullptr) {
            // Reserve the slot while holding the short metadata lock, then
            // copy outside it. RTSP readers skip a writing slot, so a large
            // MJPEG payload cannot stall the renderer or another consumer.
            slot->writing = true;
        }
    }
    if (slot == nullptr) {
        s_drops.fetch_add(1, std::memory_order_relaxed);
        return true;
    }
    const int64_t callback_start = esp_timer_get_time();
    memcpy(slot->data, frame->data, bytes);
    const uint32_t callback_us = static_cast<uint32_t>(std::max<int64_t>(0, esp_timer_get_time() - callback_start));
    uint32_t previous_max = s_callback_max_us.load(std::memory_order_relaxed);
    while (callback_us > previous_max &&
           !s_callback_max_us.compare_exchange_weak(previous_max, callback_us, std::memory_order_relaxed)) {
    }
    if (callback_us >= 20000) {
        s_callback_slow.fetch_add(1, std::memory_order_relaxed);
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        // A stop/disconnect may have happened while the payload was copied.
        // Do not publish that frame, and invalidate the overwritten snapshot
        // before another consumer can select it.
        if (!s_streaming.load(std::memory_order_acquire) || s_stop_requested.load(std::memory_order_acquire) ||
            callback_generation != s_generation.load()) {
            slot->bytes = 0;
            slot->writing = false;
            slot->ready = false;
            slot->valid = false;
            return true;
        }
        slot->bytes = bytes;
        slot->width = frame->vs_format.h_res;
        slot->height = frame->vs_format.v_res;
        slot->stride = frame->vs_format.format == UVC_VS_FORMAT_YUY2 ? slot->width * 2 : 0;
        slot->format = frame->vs_format.format;
        slot->sequence = ++s_sequence;
        slot->captured_us = callback_start;
        slot->generation = callback_generation;
        slot->ready = true;
        slot->writing = false;
        slot->valid = true;
        s_frames.fetch_add(1, std::memory_order_relaxed);
        s_last_frame_us.store(esp_timer_get_time(), std::memory_order_release);
        if (s_render_task != nullptr) {
            xTaskNotifyGive(s_render_task);
        }
    }
    return true;
}
static void stream_callback(const uvc_host_stream_event_data_t *event, void *)
{
    if (event == nullptr) {
        return;
    }
    if (event->type == UVC_HOST_DEVICE_DISCONNECTED) {
        s_connected.store(false);
        s_streaming.store(false);
        s_state.store(State::Disconnected);
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            clear_profiles_locked();
        }
        // The native driver contract supplies the handle here and requires it
        // to be closed from this callback.  The stream task observes the
        // ownership flag and will not stop/close it a second time.
        close_owned_stream(event->device_disconnected.stream_hdl, false);
        ESP_LOGW(kTag, "UVC disconnected");
    } else if (event->type == UVC_HOST_TRANSFER_ERROR) {
        s_streaming.store(false);
        // A transfer callback can race the normal stop handshake. A transfer
        // error during that handshake is not an independent capture failure;
        // stream_task owns the final stopped/disconnected state transition.
        if (!s_stop_requested.load()) {
            // The stream task owns close/reopen after a transfer error. Keep
            // the UI in the retry state during that hand-off; only malformed
            // frame data remains a terminal capture error.
            s_state.store(s_start_requested.load() ? State::Negotiating : State::Error);
        }
        ESP_LOGW(kTag, "UVC transfer error: %s", esp_err_to_name(event->transfer_error.error));
    } else if (event->type == UVC_HOST_FRAME_BUFFER_OVERFLOW || event->type == UVC_HOST_FRAME_BUFFER_UNDERFLOW) {
        s_drops.fetch_add(1, std::memory_order_relaxed);
    }
}
static void driver_callback(const uvc_host_driver_event_data_t *event, void *)
{
    if (event == nullptr || event->type != UVC_HOST_DRIVER_EVENT_DEVICE_CONNECTED) {
        return;
    }
    s_device_address.store(event->device_connected.dev_addr, std::memory_order_release);
    s_stream_index.store(event->device_connected.uvc_stream_index, std::memory_order_release);
    s_connected.store(true);
    s_state.store(State::DevicePresent);
    size_t count = event->device_connected.frame_info_num;
    uvc_host_frame_info_t list[16] = {};
    if (count > 16) {
        count = 16;
    }
    const uint8_t device_address = s_device_address.load(std::memory_order_acquire);
    const uint8_t stream_index = s_stream_index.load(std::memory_order_acquire);
    if (uvc_host_get_frame_list(device_address, stream_index, &list, &count) == ESP_OK) {
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            s_profile_count = count;
            memcpy(s_profiles, list, count * sizeof(list[0]));
        }
        ESP_LOGI(kTag, "UVC device addr=%u stream=%u frame_profiles=%u", device_address, stream_index,
                 static_cast<unsigned>(count));
        for (size_t i = 0; i < count; ++i) {
            ESP_LOGI(kTag, "UVC profile[%u] %s %ux%u interval=%" PRIu32,
                     static_cast<unsigned>(i), format_name(list[i].format), static_cast<unsigned>(list[i].h_res),
                     static_cast<unsigned>(list[i].v_res), static_cast<uint32_t>(list[i].default_interval));
        }
    } else {
        std::lock_guard<std::mutex> lock(s_mutex);
        clear_profiles_locked();
        ESP_LOGW(kTag, "UVC frame profile query failed; stale profiles cleared");
    }
}
static bool open_profile(enum uvc_host_stream_format format, uint32_t width, uint32_t height, float fps,
                         uvc_host_stream_hdl_t *stream)
{
    uvc_host_stream_config_t config = {};
    config.event_cb = stream_callback;
    config.frame_cb = frame_callback;
    config.usb.dev_addr = s_device_address.load(std::memory_order_acquire);
    config.usb.uvc_stream_index = s_stream_index.load(std::memory_order_acquire);
    config.vs_format.h_res = width;
    config.vs_format.v_res = height;
    config.vs_format.fps = fps;
    config.vs_format.format = format;
    config.advanced.number_of_frame_buffers = kSlotCount;
    config.advanced.frame_size = 0;
    config.advanced.frame_heap_caps = MALLOC_CAP_SPIRAM;
    config.advanced.number_of_urbs = 3;
    config.advanced.urb_size = 0;
    return uvc_host_stream_open(&config, pdMS_TO_TICKS(3000), stream) == ESP_OK;
}

static bool stream_can_continue()
{
    return !s_stop_requested.load() && s_start_requested.load() && s_connected.load();
}

static bool open_fallback_profile(enum uvc_host_stream_format format, uvc_host_stream_hdl_t *stream)
{
    if (!stream_can_continue()) {
        return false;
    }
    uvc_host_frame_info_t profiles[16] = {};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        count = s_profile_count;
        memcpy(profiles, s_profiles, count * sizeof(profiles[0]));
    }
    const uvc_host_frame_info_t *best = nullptr;
    uint64_t best_score = UINT64_MAX;
    for (size_t i = 0; i < count; ++i) {
        const auto &profile = profiles[i];
        if (profile.format != format || profile.h_res > kWidth || profile.v_res > kHeight ||
            profile.h_res == 0 || profile.v_res == 0) {
            continue;
        }
        const float fps = profile.default_interval == 0 ? 0.0F : 10000000.0F / profile.default_interval;
        if (profile.h_res == kWidth && profile.v_res == kHeight &&
            (fps == 0.0F || static_cast<unsigned>(fps + 0.5F) == CONFIG_TAB5_UVC_FPS)) {
            continue;
        }
        const uint64_t resolution_delta = static_cast<uint64_t>(kWidth - profile.h_res) * kHeight +
                                          static_cast<uint64_t>(kHeight - profile.v_res) * kWidth;
        const uint64_t fps_delta = fps == 0.0F ? 0 :
            static_cast<uint64_t>(std::abs(fps - static_cast<float>(CONFIG_TAB5_UVC_FPS)) * 1000.0F);
        const uint64_t score = resolution_delta * 1000 + fps_delta;
        if (score < best_score) {
            best = &profile;
            best_score = score;
        }
    }
    if (best == nullptr) {
        return false;
    }
    const float fps = best->default_interval == 0 ? 0.0F : 10000000.0F / best->default_interval;
    ESP_LOGI(kTag, "UVC fallback %s %ux%u@%.2f", format_name(format),
             static_cast<unsigned>(best->h_res), static_cast<unsigned>(best->v_res),
             static_cast<double>(fps));
    if (!stream_can_continue()) {
        return false;
    }
    return open_profile(format, best->h_res, best->v_res, fps, stream);
}

static bool start_opened_stream(uvc_host_stream_hdl_t *stream)
{
    if (stream == nullptr || *stream == nullptr) {
        return false;
    }
    if (!stream_can_continue()) {
        uvc_host_stream_close(*stream);
        *stream = nullptr;
        return false;
    }
    if (!mark_stream_open(*stream)) {
        ESP_LOGE(kTag, "another UVC stream still owns the driver");
        uvc_host_stream_close(*stream);
        *stream = nullptr;
        s_restart_blocked.store(true);
        return false;
    }
    if (!stream_can_continue()) {
        close_owned_stream(*stream, false);
        *stream = nullptr;
        return false;
    }
    uvc_host_desc_print(*stream);
    if (!stream_can_continue()) {
        close_owned_stream(*stream, false);
        *stream = nullptr;
        return false;
    }
    const esp_err_t result = uvc_host_stream_start(*stream);
    if (result != ESP_OK) {
        ESP_LOGW(kTag, "UVC stream start failed: %s; trying the next profile", esp_err_to_name(result));
        close_owned_stream(*stream, false);
        *stream = nullptr;
        return false;
    }
    if (!stream_can_continue()) {
        close_owned_stream(*stream, true);
        *stream = nullptr;
        return false;
    }
    return true;
}

static bool try_stream_profile(enum uvc_host_stream_format format, bool fallback,
                               uvc_host_stream_hdl_t *stream)
{
    if (!stream_can_continue()) {
        return false;
    }
    bool opened = false;
    if (fallback) {
        opened = open_fallback_profile(format, stream);
    } else {
        opened = open_profile(format, kWidth, kHeight,
                              static_cast<float>(CONFIG_TAB5_UVC_FPS), stream);
    }
    if (!opened) {
        return false;
    }
    if (!stream_can_continue()) {
        if (*stream != nullptr) {
            uvc_host_stream_close(*stream);
            *stream = nullptr;
        }
        return false;
    }
    return start_opened_stream(stream);
}

static void render_task(void *)
{
    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(500));
        {
            std::lock_guard<std::mutex> lock(s_mutex);
            for (auto &slot : s_slots) {
                if (!slot.writing && !slot.copying) slot.ready = false;
            }
        }
        if (!s_capturing.load() || s_stop_requested.load()) {
            s_render_stop_complete.store(true);
            maybe_finish_stop();
            xSemaphoreGive(s_stop_done);
        }
    }
}
static void stream_task(void *)
{
    while (true) {
        xSemaphoreTake(s_start_event, portMAX_DELAY);
        if (!s_start_requested.load()) {
            continue;
        }
        while (s_start_requested.load() && !s_stop_requested.load()) {
            if (!s_connected.load()) {
                s_state.store(State::DeviceAbsent);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (stream_handle_is_owned()) {
                s_state.store(State::Negotiating);
                ESP_LOGW(kTag, "UVC stream handle still owned; waiting before restart");
                vTaskDelay(pdMS_TO_TICKS(250));
                continue;
            }
            // A previous ownership collision is recoverable once its exact
            // handle has gone away.  Keep the guard while it is live, but do
            // not turn this transient hand-off into a terminal UI error.
            if (s_restart_blocked.exchange(false, std::memory_order_acq_rel)) {
                ESP_LOGW(kTag, "UVC stream ownership cleared; retrying negotiation");
            }
            s_state.store(State::Negotiating);
            uvc_host_stream_hdl_t stream = nullptr;
            bool stream_started = false;
            if (stream_can_continue()) {
                stream_started = try_stream_profile(UVC_VS_FORMAT_MJPEG, false, &stream);
            }
            if (!stream_started && stream_can_continue()) {
                stream_started = try_stream_profile(UVC_VS_FORMAT_MJPEG, true, &stream);
            }
            if (!stream_started && stream_can_continue()) {
                stream_started = try_stream_profile(UVC_VS_FORMAT_YUY2, false, &stream);
            }
            if (!stream_started && stream_can_continue()) {
                stream_started = try_stream_profile(UVC_VS_FORMAT_YUY2, true, &stream);
            }
            if (!stream_started) {
                if (!stream_can_continue()) {
                    break;
                }
                if (s_restart_blocked.load()) {
                    // `mark_stream_open()` can observe an old handle while
                    // the disconnect callback is closing it.  Keep this
                    // session alive and wait for the exact owner to clear;
                    // opening another stream here would reintroduce the UVC
                    // callback/use-after-close race.
                    s_state.store(State::Negotiating);
                    vTaskDelay(pdMS_TO_TICKS(250));
                    continue;
                }
                s_state.store(State::DevicePresent);
                vTaskDelay(pdMS_TO_TICKS(1000));
                continue;
            }
            if (!stream_can_continue()) {
                close_owned_stream(stream, true);
                break;
            }
            uvc_host_stream_format_t actual_format = {};
            if (uvc_host_stream_format_get(stream, &actual_format) == ESP_OK) {
                {
                    std::lock_guard<std::mutex> lock(s_mutex);
                    s_negotiated_format = actual_format;
                }
                ESP_LOGI(kTag, "UVC negotiated %s %ux%u@%.2f",
                         format_name(actual_format.format),
                         static_cast<unsigned>(actual_format.h_res),
                         static_cast<unsigned>(actual_format.v_res),
                         static_cast<double>(actual_format.fps));
            } else {
                ESP_LOGW(kTag, "UVC negotiated format query failed");
            }
            if (!stream_can_continue()) {
                close_owned_stream(stream, true);
                break;
            }
            s_generation.fetch_add(1);
            s_streaming.store(true);
            s_state.store(State::Streaming);
            s_last_frame_us.store(esp_timer_get_time(), std::memory_order_release);
            ESP_LOGI(kTag, "UVC streaming requested %ux%u@%u",
                     static_cast<unsigned>(kWidth), static_cast<unsigned>(kHeight),
                     static_cast<unsigned>(CONFIG_TAB5_UVC_FPS));
            while (s_start_requested.load() && !s_stop_requested.load() && s_connected.load() && s_streaming.load()) {
                const int64_t now_us = esp_timer_get_time();
                const int64_t last_frame_us = s_last_frame_us.load(std::memory_order_acquire);
                if (last_frame_us != 0 && now_us - last_frame_us >= kFrameStallTimeoutUs) {
                    s_watchdog_restarts.fetch_add(1, std::memory_order_relaxed);
                    ESP_LOGW(kTag, "UVC frame watchdog expired; restarting stalled stream");
                    s_streaming.store(false);
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            s_streaming.store(false);
            close_owned_stream(stream, true);
            if (s_frame_format_error.load(std::memory_order_acquire)) {
                s_start_requested.store(false);
                s_capturing.store(false);
                s_state.store(State::Error);
                break;
            }
            if (!s_stop_requested.load()) {
                s_state.store(s_connected.load() ? State::DevicePresent : State::Disconnected);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
        }
        s_streaming.store(false);
        if (s_stop_requested.load()) {
            s_start_requested.store(false);
            s_state.store(s_connected.load() ? State::DevicePresent : State::Disconnected);
            s_stream_stop_complete.store(true);
            maybe_finish_stop();
            xSemaphoreGive(s_stream_done);
        }
    }
}

static void free_init_resources()
{
    for (auto &slot : s_slots) {
        if (slot.data != nullptr) {
            heap_caps_free(slot.data);
            slot.data = nullptr;
        }
    }
    if (s_start_event != nullptr) {
        vSemaphoreDelete(s_start_event);
        s_start_event = nullptr;
    }
    if (s_stop_done != nullptr) {
        vSemaphoreDelete(s_stop_done);
        s_stop_done = nullptr;
    }
    if (s_stream_done != nullptr) {
        vSemaphoreDelete(s_stream_done);
        s_stream_done = nullptr;
    }
}
} // namespace

extern "C" bool hal_uvc_init(void)
{
    if (s_initialized.load()) {
        return true;
    }
    for (auto &slot : s_slots) {
        slot.data = static_cast<uint8_t *>(heap_caps_malloc(kMaxFrameBytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (slot.data == nullptr) {
            free_init_resources();
            s_state.store(State::Error);
            return false;
        }
    }
    s_start_event = xSemaphoreCreateBinary();
    s_stop_done = xSemaphoreCreateBinary();
    s_stream_done = xSemaphoreCreateBinary();
    if (s_start_event == nullptr || s_stop_done == nullptr ||
        s_stream_done == nullptr) {
        free_init_resources();
        s_state.store(State::Error);
        return false;
    }
    const uvc_host_driver_config_t config = {
        .driver_task_stack_size = 4096,
        .driver_task_priority = 6,
        .xCoreID = 1,
        .create_background_task = true,
        .event_cb = driver_callback,
        .user_ctx = nullptr,
    };
    const esp_err_t result = uvc_host_install(&config);
    if (result != ESP_OK) {
        ESP_LOGE(kTag, "uvc_host_install failed: %s", esp_err_to_name(result));
        free_init_resources();
        s_state.store(State::Error);
        return false;
    }
    // UVC keeps two long-lived worker stacks.  Keep their TCBs in internal RAM,
    // but put the stacks in PSRAM so the P4 H.264 encoder can obtain its
    // contiguous internal reference frame when RTSP starts later.
    constexpr UBaseType_t kExternalStackCaps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    if (xTaskCreatePinnedToCoreWithCaps(stream_task, "uvc_stream", 8 * 1024, nullptr, 5, &s_stream_task, 1,
                                        kExternalStackCaps) != pdPASS) {
        uvc_host_uninstall();
        free_init_resources();
        s_state.store(State::Error);
        return false;
    }
    if (xTaskCreatePinnedToCoreWithCaps(render_task, "uvc_render", 8 * 1024, nullptr, 4, &s_render_task, 1,
                                        kExternalStackCaps) != pdPASS) {
        vTaskDelete(s_stream_task);
        s_stream_task = nullptr;
        uvc_host_uninstall();
        free_init_resources();
        s_state.store(State::Error);
        return false;
    }
    s_initialized.store(true);
    s_state.store(State::DriverReady);
    ESP_LOGI(kTag, "native USB Host UVC driver ready (MJPEG then YUY2)");
    return true;
}
extern "C" bool hal_uvc_is_available(void) { return s_initialized.load(); }
extern "C" hal_uvc_state_t hal_uvc_state(void) { return static_cast<hal_uvc_state_t>(s_state.load()); }
extern "C" bool hal_uvc_is_connected(void) { return s_connected.load(); }
extern "C" bool hal_uvc_is_streaming(void) { return s_streaming.load(); }
extern "C" const char *hal_uvc_name(void) { return "native-usb-host-uvc"; }
extern "C" bool hal_uvc_get_metrics(hal_uvc_metrics_t *metrics)
{
    if (metrics == nullptr) {
        return false;
    }
    memset(metrics, 0, sizeof(*metrics));
    metrics->state = static_cast<hal_uvc_state_t>(s_state.load(std::memory_order_acquire));
    metrics->initialized = s_initialized.load(std::memory_order_acquire);
    metrics->connected = s_connected.load(std::memory_order_acquire);
    metrics->capturing = s_capturing.load(std::memory_order_acquire);
    metrics->streaming = s_streaming.load(std::memory_order_acquire);
    metrics->stop_pending = s_stop_pending.load(std::memory_order_acquire);
    metrics->restart_blocked = s_restart_blocked.load(std::memory_order_acquire);
    metrics->frames = s_frames.load(std::memory_order_relaxed);
    metrics->drops = s_drops.load(std::memory_order_relaxed);
    metrics->active_copies = s_active_copies.load(std::memory_order_acquire);
    metrics->callback_max_us = s_callback_max_us.load(std::memory_order_relaxed);
    metrics->callback_slow = s_callback_slow.load(std::memory_order_relaxed);
    metrics->preview_frames = s_preview_frames.load(std::memory_order_relaxed);
    metrics->preview_decode_errors = s_preview_decode_errors.load(std::memory_order_relaxed);
    metrics->watchdog_restarts = s_watchdog_restarts.load(std::memory_order_relaxed);

    // Slot flags and task handles are protected by the same short metadata
    // mutex used by the broker. No frame pointer is returned or retained.
    std::lock_guard<std::mutex> lock(s_mutex);
    for (size_t i = 0; i < kSlotCount; ++i) {
        const uint32_t bit = 1U << i;
        metrics->slot_writing_mask |= s_slots[i].writing ? bit : 0U;
        metrics->slot_ready_mask |= s_slots[i].ready ? bit : 0U;
        metrics->slot_rendering_mask |= s_slots[i].rendering ? bit : 0U;
        metrics->slot_copying_mask |= s_slots[i].copying ? bit : 0U;
        metrics->slot_valid_mask |= s_slots[i].valid ? bit : 0U;
    }
    metrics->stream_task_stack_hwm = s_stream_task != nullptr
                                         ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_stream_task))
                                         : 0;
    metrics->render_task_stack_hwm = s_render_task != nullptr
                                         ? static_cast<uint32_t>(uxTaskGetStackHighWaterMark(s_render_task))
                                         : 0;
    return true;
}
extern "C" bool hal_uvc_copy_latest_frame(uint32_t last_sequence, uint8_t *dst, size_t capacity,
                                            hal_uvc_frame_info_t *info)
{
    if (dst == nullptr || info == nullptr || !s_initialized.load() || !s_streaming.load()) {
        return false;
    }
    Slot *latest = nullptr;
    size_t bytes = 0;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto &candidate : s_slots) {
            if (!candidate.valid || candidate.writing || candidate.copying || candidate.sequence <= last_sequence ||
                candidate.generation != s_generation.load() ||
                candidate.data == nullptr || candidate.bytes == 0) {
                continue;
            }
            if (latest == nullptr || candidate.sequence > latest->sequence) {
                latest = &candidate;
            }
        }
        if (latest == nullptr || latest->bytes > capacity) {
            return false;
        }
        latest->copying = true;
        s_active_copies.fetch_add(1, std::memory_order_acq_rel);
        bytes = latest->bytes;
    }

    // Reserve the slot under the metadata lock but copy outside it. The UVC
    // callback can continue publishing into another slot while RTSP copies a
    // large MJPEG payload; callback and RTSP no longer serialize on memcpy.
    memcpy(dst, latest->data, bytes);
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        info->bytes = bytes;
        info->width = latest->width;
        info->height = latest->height;
        info->stride = latest->stride;
        info->sequence = latest->sequence;
        info->captured_us = latest->captured_us;
        info->generation = latest->generation;
        info->format = latest->format == UVC_VS_FORMAT_MJPEG ? HAL_UVC_FRAME_MJPEG :
                       latest->format == UVC_VS_FORMAT_YUY2 ? HAL_UVC_FRAME_YUY2 : HAL_UVC_FRAME_UNKNOWN;
        latest->copying = false;
    }
    s_active_copies.fetch_sub(1, std::memory_order_acq_rel);
    maybe_finish_stop();
    return s_streaming.load() && info->generation == s_generation.load();
}
extern "C" bool hal_uvc_start(void)
{
    if (!hal_uvc_is_available()) {
        ESP_LOGW(kTag, "UVC start rejected: driver unavailable");
        return false;
    }
    if (s_capturing.load()) {
        ESP_LOGW(kTag, "UVC start rejected: capture already running");
        return false;
    }
    if (s_stop_pending.load()) {
        ESP_LOGW(kTag, "UVC start deferred: previous stream is still stopping");
        return false;
    }
    if (s_restart_blocked.load()) {
        if (stream_handle_is_owned()) {
            ESP_LOGW(kTag, "UVC start waiting: stale stream ownership is still live");
        } else {
            // The close callback cleared the handle but the error flag was
            // read before that callback completed. It is safe to recover now.
            s_restart_blocked.store(false, std::memory_order_release);
        }
    }
    if (stream_handle_is_owned()) {
        ESP_LOGW(kTag, "UVC start waiting: stream handle is still owned");
    }
    // A new explicit start is the recovery action after a malformed frame;
    // the failed session itself remains Error until this point.
    s_frame_format_error.store(false, std::memory_order_release);
    if (s_state.load() == State::Error) {
        s_state.store(s_connected.load() ? State::DevicePresent : State::DeviceAbsent);
    }
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto &slot : s_slots) {
            slot.ready = false;
            slot.rendering = false;
            slot.writing = false;
            slot.copying = false;
            slot.valid = false;
        }
    }
    s_frames.store(0, std::memory_order_relaxed);
    s_drops.store(0, std::memory_order_relaxed);
    s_callback_max_us.store(0, std::memory_order_relaxed);
    s_callback_slow.store(0, std::memory_order_relaxed);
    s_preview_frames.store(0, std::memory_order_relaxed);
    s_preview_decode_errors.store(0, std::memory_order_relaxed);
    s_watchdog_restarts.store(0, std::memory_order_relaxed);
    s_last_frame_us.store(0, std::memory_order_release);
    s_last_preview_us.store(0, std::memory_order_release);
    if (s_stop_done != nullptr) {
        xSemaphoreTake(s_stop_done, 0);
    }
    if (s_stream_done != nullptr) {
        xSemaphoreTake(s_stream_done, 0);
    }
    s_stop_requested.store(false);
    s_start_requested.store(true);
    s_capturing.store(true);
    if (s_render_task != nullptr) {
        xTaskNotifyGive(s_render_task);
    }
    xSemaphoreGive(s_start_event);
    return true;
}
extern "C" uint32_t hal_uvc_generation(void) { return s_generation.load(); }
extern "C" void hal_uvc_stop(void)
{
    if (!s_initialized.load() || (!s_capturing.load() && !s_stop_pending.load())) {
        return;
    }
    if (s_stop_pending.exchange(true)) {
        return;
    }
    s_stream_stop_complete.store(false);
    s_render_stop_complete.store(false);
    s_stop_requested.store(true);
    s_capturing.store(false);
    // Detach before notifying the renderer. The caller may hold the LVGL
    // lock, and waiting here for a task that needs that lock deadlocks.
    if (s_render_task != nullptr) {
        xTaskNotifyGive(s_render_task);
    }
    xSemaphoreGive(s_start_event);
}
extern "C" bool hal_uvc_is_capturing(void)
{
    return s_capturing.load() || s_stop_pending.load();
}
