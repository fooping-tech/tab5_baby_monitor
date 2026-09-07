#include "live_services.hpp"
#include "bsp/m5stack_tab5.h"
#include "hal/hal_uvc.h"
#include "frame_convert.hpp"
#include "jpeg_decoder.hpp"
#include "rtsp/rtsp_server.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "esp_log.h"
#include "freertos/idf_additions.h"
#include <ctime>
#include <algorithm>
#include <cstring>
#include <atomic>

LV_FONT_DECLARE(lv_font_camera_clock_64);
LV_FONT_DECLARE(lv_font_camera_clock_time_88);

namespace {
lv_obj_t *canvas;
lv_obj_t *clock_label;
lv_obj_t *date_label;
lv_obj_t *weekday_label;
lv_obj_t *status_label;
uint16_t *pixels;
uint16_t *render_pixels;
std::atomic<uint32_t> preview_frames{0};
// Accounting for every iteration of the preview loop, so a stalled camera
// image can be attributed instead of guessed at.
std::atomic<uint32_t> preview_no_new_frame{0};   // the HAL had nothing newer
std::atomic<uint32_t> preview_too_old{0};        // arrived already past its age limit
std::atomic<uint32_t> preview_decode_failed{0};  // decoder refused or timed out
std::atomic<uint32_t> preview_blanked{0};        // canvas cleared as stale
std::atomic<uint32_t> preview_borrowed{0};       // reused the RTSP pipeline's decode
// RGB888 comes from our own decode, BGR888 from the RTSP pipeline's.
enum class SourcePixels { Rgb888, Bgr888 };
std::atomic<bool> usb_live{false};
std::atomic<const char *> presence_text{"unknown"};
constexpr size_t payload_capacity = 1024 * 1024;
constexpr int64_t kPreviewMaxAgeUs = CONFIG_TAB5_PREVIEW_MAX_AGE_MS * 1000LL;
// A frame is only put on screen while it is comfortably inside the age limit,
// but the canvas is not blanked until the limit itself is passed. Using one
// threshold for both made the two fight: a borrowed frame accepted at 1.4 s
// old was blanked 0.1 s later, which is the camera image flicking to black
// every few seconds. The gap between the two is the hysteresis.
constexpr int64_t kPreviewDisplayAgeUs = kPreviewMaxAgeUs / 2;
// camera_clock_font_64.c / camera_clock_time_88.c were generated with
// "--symbols /:0123456789", so "/", ":" and the digits are every glyph they
// have. The old "----/--/--" and "--:--:--" placeholders asked those fonts
// for '-', which LVGL draws as its missing-glyph box: that is the garbled
// clock seen for the first seconds after boot. Build the placeholder from
// glyphs the fonts actually carry. Switching to a second face instead linked
// a 48 px montserrat, grew the image by 324 KiB and cost 96 KiB of free
// PSRAM, which this board cannot spare next to the 2 MiB contiguous RTSP
// input buffer. The status panel's "CLOCK //" line says whether the digits
// mean anything yet.
constexpr char kDatePlaceholder[] = "0000/00/00";
constexpr char kTimePlaceholder[] = "00:00:00";
// The weekday label uses montserrat, which does carry '-'.
constexpr char kWeekdayPlaceholder[] = "(--)";

lv_obj_t *label(lv_obj_t *parent, int top, const char *text, uint32_t color)
{
    auto *object = lv_label_create(parent);
    lv_obj_set_width(object, 680);
    lv_obj_set_pos(object, 20, top);
    lv_obj_set_style_text_align(object, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(object, lv_color_hex(color), 0);
    lv_label_set_text(object, text);
    return object;
}

void update_labels(lv_timer_t *)
{
    const time_t now = time(nullptr);
    struct tm local{};
    localtime_r(&now, &local);
    const bool digits_are_real = local.tm_year >= 124;
    if (digits_are_real) {
        char text[64];
        strftime(text, sizeof(text), "%Y/%m/%d", &local);
        lv_label_set_text(date_label, text);
        const char *weekdays[] = {"(SUN)", "(MON)", "(TUE)", "(WED)", "(THU)", "(FRI)", "(SAT)"};
        lv_label_set_text(weekday_label, weekdays[local.tm_wday]);
        strftime(text, sizeof(text), "%H:%M:%S", &local);
        lv_label_set_text(clock_label, text);
    } else {
        lv_label_set_text(date_label, kDatePlaceholder);
        lv_label_set_text(weekday_label, kWeekdayPlaceholder);
        lv_label_set_text(clock_label, kTimePlaceholder);
    }
    lv_label_set_text_fmt(status_label, "USB // %s\nPERSON // %s\nPOSTURE // unknown\nCLOCK // %s",
                          usb_live.load(std::memory_order_relaxed) ? "LIVE" : "OFFLINE",
                          presence_text.load(std::memory_order_relaxed),
                          edge_clock_is_synchronized() ? "NTP" : (digits_are_real ? "RTC-ONLY" : "UNSET"));
}

void preview(void *)
{
    auto *payload = static_cast<uint8_t *>(heap_caps_malloc(payload_capacity, MALLOC_CAP_SPIRAM));
    auto *rgb = edge_jpeg_rgb_buffer_alloc(CONFIG_TAB5_UVC_WIDTH * CONFIG_TAB5_UVC_HEIGHT * 3);
    // Destination-to-source column map, rebuilt whenever the frame geometry
    // changes. At most one entry per canvas column.
    auto *column_source = static_cast<uint16_t *>(heap_caps_calloc(720, sizeof(uint16_t),
                                                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    uint32_t sequence = 0;
    int64_t displayed_us = 0;
    bool has_preview = false;
    // Fixed 5 Hz cadence. vTaskDelay() sleeps for its full period *after* the
    // work, so ~100 ms of copy/decode/scale turned a 200 ms delay into a
    // 302 ms loop and capped the preview at 3.3 fps. xTaskDelayUntil() takes
    // the work out of the period instead.
    TickType_t next_wake = xTaskGetTickCount();
    constexpr TickType_t preview_period = pdMS_TO_TICKS(200);
    while (payload && rgb && column_source) {
        hal_uvc_frame_info_t frame{};
        const uint8_t *decoded = nullptr;
        SourcePixels pixels_are = SourcePixels::Rgb888;
        bool borrowed = false;
        // Prefer the frame the RTSP pipeline has already put through the
        // hardware JPEG engine. Decoding it again here only queues behind that
        // same engine: measured 133-195 ms per call against a 200 ms period.
        // The borrow is held for the scale loop and nothing else.
        const uint8_t *shared = nullptr;
        hal_uvc_frame_info_t shared_info{};
        if (rtsp_borrow_decoded_frame(sequence, &shared, &shared_info, 20)) {
            // Copy out and release at once. Holding the borrow across the
            // scale would block the RTSP pipeline's next decode for as long
            // as this task takes, and the blank path below would hold it
            // across bsp_display_lock(), which waits on the LVGL task.
            // 900 KiB of PSRAM copy costs about 10 ms; the alternative is a
            // hardware decode this frame does not need.
            memcpy(rgb, shared, static_cast<size_t>(shared_info.width) * shared_info.height * 3);
            rtsp_release_decoded_frame();
            frame = shared_info;
            sequence = frame.sequence;
            decoded = rgb;
            pixels_are = SourcePixels::Bgr888;  // the encoder path decodes BGR
            borrowed = true;
            ++preview_borrowed;
        }
        const bool got_frame =
            borrowed || hal_uvc_copy_latest_frame(sequence, payload, payload_capacity, &frame);
        if (!got_frame) {
            ++preview_no_new_frame;
        } else if (borrowed) {
            // Already decoded; freshness is still checked below.
        } else {
            sequence = frame.sequence;
            // Check the age before decoding, not after. The RTSP path drives
            // the same hardware JPEG decoder at CONFIG_TAB5_RTSP_FPS, so a
            // preview decode can sit behind it long enough that the frame is
            // already too old by the time it completes - and is then thrown
            // away, leaving the canvas to blank itself. Skipping the decode
            // instead keeps the last good picture on screen and stops
            // competing for the decoder we are losing anyway.
            const bool fresh_enough =
                esp_timer_get_time() - frame.captured_us < kPreviewDisplayAgeUs;
            if (!fresh_enough) ++preview_too_old;
            if (fresh_enough && frame.width && frame.height && frame.width <= CONFIG_TAB5_UVC_WIDTH &&
                frame.height <= CONFIG_TAB5_UVC_HEIGHT) {
                if (frame.format == HAL_UVC_FRAME_MJPEG) {
                    // RGB888 with JPEG_DEC_RGB_ELEMENT_ORDER_RGB, packed to
                    // RGB565 below. Decoding straight to RGB565 halves the
                    // bytes but the driver applies a byte scramble for that
                    // combination (jpeg_decode.c: DMA2D_SCRAMBLE_ORDER_BYTE2_0_1)
                    // and the colours came out wrong on the panel.
                    if (edge_decode_jpeg_rgb888(payload, frame.bytes, frame.width, frame.height, rgb,
                                                CONFIG_TAB5_UVC_WIDTH * CONFIG_TAB5_UVC_HEIGHT * 3)) {
                        decoded = rgb;
                        pixels_are = SourcePixels::Rgb888;
                    }
                } else if (frame.format == HAL_UVC_FRAME_YUY2 &&
                           baby_edge::yuy2_to_rgb(payload, frame.bytes, frame.width, frame.height,
                               frame.stride, rgb, CONFIG_TAB5_UVC_WIDTH * CONFIG_TAB5_UVC_HEIGHT * 3)) {
                    decoded = rgb;
                    pixels_are = SourcePixels::Rgb888;
                }
                if (!decoded) ++preview_decode_failed;
            }
        }
        if (decoded && frame.generation == hal_uvc_generation() && hal_uvc_is_streaming() &&
            esp_timer_get_time() - frame.captured_us < kPreviewDisplayAgeUs) {
            const int width = 720;
            const int height = std::min(640, static_cast<int>(720 * frame.height / frame.width));
            const int fitted_width = height * frame.width / frame.height;
            const int top = (640 - height) / 2;
            const int left = (width - fitted_width) / 2;
            const auto *source = static_cast<const uint8_t *>(decoded);
            // The source column for a given destination column is the same on
            // every row, so it is computed once per frame instead of once per
            // pixel. Together with hoisting the row term out of the inner loop
            // this removes about 920000 integer divisions per frame: the old
            // form recomputed "row * frame.height / height" for every column
            // and left the preview loop taking 535 ms per iteration against
            // its 200 ms period, which is what starved the camera image.
            for (int column = 0; column < fitted_width; ++column) {
                column_source[column] = static_cast<uint16_t>(column * frame.width / fitted_width);
            }
            // Clear only the letterbox bars; the picture overwrites the rest.
            if (top > 0) {
                memset(render_pixels, 0, static_cast<size_t>(top) * width * sizeof(uint16_t));
                memset(render_pixels + static_cast<size_t>(top + height) * width, 0,
                       static_cast<size_t>(640 - top - height) * width * sizeof(uint16_t));
            }
            for (int row = 0; row < height; ++row) {
                const uint8_t *source_row =
                    source + static_cast<size_t>(row * frame.height / height) * frame.width * 3;
                uint16_t *destination = render_pixels + static_cast<size_t>(row + top) * width;
                if (left > 0) {
                    memset(destination, 0, static_cast<size_t>(left) * sizeof(uint16_t));
                    memset(destination + left + fitted_width, 0,
                           static_cast<size_t>(width - left - fitted_width) * sizeof(uint16_t));
                }
                destination += left;
                if (pixels_are == SourcePixels::Bgr888) {
                    for (int column = 0; column < fitted_width; ++column) {
                        const uint8_t *pixel = source_row + static_cast<size_t>(column_source[column]) * 3;
                        destination[column] = static_cast<uint16_t>(((pixel[2] >> 3) << 11) |
                                                                    ((pixel[1] >> 2) << 5) | (pixel[0] >> 3));
                    }
                } else {
                    for (int column = 0; column < fitted_width; ++column) {
                        const uint8_t *pixel = source_row + static_cast<size_t>(column_source[column]) * 3;
                        destination[column] = static_cast<uint16_t>(((pixel[0] >> 3) << 11) |
                                                                    ((pixel[1] >> 2) << 5) | (pixel[2] >> 3));
                    }
                }
            }
            const bool locked = bsp_display_lock(0);
            if (locked) {
                // Hand the finished frame to LVGL by swapping the two canvas
                // buffers. Copying 900 KiB of PSRAM while holding the LVGL
                // lock stalled rendering and doubled the PSRAM traffic that
                // the MIPI-DSI scan-out competes for.
                std::swap(pixels, render_pixels);
                lv_canvas_set_buffer(canvas, pixels, 720, 640, LV_COLOR_FORMAT_RGB565);
                lv_obj_invalidate(canvas);
                displayed_us = frame.captured_us;
                ++preview_frames;
                has_preview = true;
                bsp_display_unlock();
                lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
            }
        } else if (has_preview && (!hal_uvc_is_streaming() ||
                                   esp_timer_get_time() - displayed_us > kPreviewMaxAgeUs)) {
            memset(render_pixels, 0, 720 * 640 * sizeof(uint16_t));
            if (bsp_display_lock(0)) {
                std::swap(pixels, render_pixels);
                lv_canvas_set_buffer(canvas, pixels, 720, 640, LV_COLOR_FORMAT_RGB565);
                lv_obj_invalidate(canvas);
                has_preview = false;
                ++preview_blanked;
                bsp_display_unlock();
                lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
            }
        }
        xTaskDelayUntil(&next_wake, preview_period);
    }
    heap_caps_free(payload);
    heap_caps_free(rgb);
    heap_caps_free(column_source);
    vTaskDeleteWithCaps(nullptr);
}
}

esp_err_t edge_ui_start()
{
    bsp_reset_tp();
    bsp_display_cfg_t config{};
    config.lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    config.buffer_size = BSP_LCD_H_RES * 120;
    config.double_buffer = true;
    config.flags.buff_spiram = true;
    config.flags.buff_dma = true;
    auto *display = bsp_display_start_with_config(&config);
    if (!display) return ESP_FAIL;
    constexpr uint32_t preview_buffer_caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    pixels = static_cast<uint16_t *>(heap_caps_calloc(720 * 640, sizeof(uint16_t), preview_buffer_caps));
    if (!pixels) return ESP_ERR_NO_MEM;
    render_pixels = static_cast<uint16_t *>(heap_caps_calloc(720 * 640, sizeof(uint16_t), preview_buffer_caps));
    if (!render_pixels) return ESP_ERR_NO_MEM;
    if (!bsp_display_lock(1000)) return ESP_ERR_TIMEOUT;
    lv_display_set_rotation(display, LV_DISPLAY_ROTATION_0);
    auto *root = lv_screen_active();
    ESP_LOGI("edge_ui", "logical display=%d x %d",
             static_cast<int>(lv_display_get_horizontal_resolution(display)),
             static_cast<int>(lv_display_get_vertical_resolution(display)));
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, 720, 1280);
    lv_obj_set_style_bg_color(root, lv_color_hex(0x050A12), 0);
    lv_obj_set_style_pad_all(root, 0, 0);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    canvas = lv_canvas_create(root);
    lv_canvas_set_buffer(canvas, pixels, 720, 640, LV_COLOR_FORMAT_RGB565);
    lv_obj_set_pos(canvas, 0, 0);
    auto *panel = lv_obj_create(root);
    lv_obj_set_pos(panel, 16, 660);
    lv_obj_set_size(panel, 688, 512);
    lv_obj_set_style_bg_color(panel, lv_color_hex(0x08141E), 0);
    lv_obj_set_style_border_color(panel, lv_color_hex(0x1D6473), 0);
    lv_obj_set_style_radius(panel, 16, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    label(root, 16, "CAM // LIVE                    RTSP // H.264", 0x65F5FF);
    label(root, 690, "LOCAL // TIME", 0x65F5FF);
    date_label = label(root, 724, kDatePlaceholder, 0xB5D7E1);
    lv_obj_set_width(date_label, 720);
    lv_obj_set_style_text_font(date_label, &lv_font_camera_clock_64, 0);
    lv_obj_set_style_text_letter_space(date_label, 2, 0);
    weekday_label = label(root, 800, kWeekdayPlaceholder, 0xFF69DD);
    lv_obj_set_width(weekday_label, 720);
    lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(weekday_label, 2, 0);
    clock_label = label(root, 850, kTimePlaceholder, 0x65F5FF);
    lv_obj_set_pos(clock_label, 8, 850);
    lv_obj_set_width(clock_label, 704);
    lv_obj_set_style_text_font(clock_label, &lv_font_camera_clock_time_88, 0);
    status_label = label(root, 1050, "AI // unknown", 0x65F5FF);
    label(root, 1192, "TAB5 // CAMERA CLOCK - EDGE AI PoC", 0xD83AAE);
    lv_timer_create(update_labels, 1000, nullptr);
    update_labels(nullptr);
    bsp_display_unlock();
    bsp_display_backlight_on();
    // Priority 3, above the inference worker. Both used to run at 2, so the
    // scheduler time-sliced a 5 Hz consumer against a task that computes for
    // 2.4-7.0 s at a stretch: measured 3 of 20 one-second samples with zero
    // preview frames and 8 of 20 at one frame or less, which is what blanked
    // the camera image every few seconds. Left unpinned; core 1 is reserved
    // for the camera tasks that hal_uvc.cpp pins there.
    return xTaskCreateWithCaps(preview, "edge_preview", 8192, nullptr, 3, nullptr,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void edge_ui_status(const char *presence)
{
    ESP_LOGI("edge_ui",
             "preview_frames=%lu no_new=%lu too_old=%lu decode_failed=%lu blanked=%lu "
             "jpeg_lock_timeouts=%lu jpeg_decode_failures=%lu "
             "borrowed=%lu",
             static_cast<unsigned long>(preview_frames.load()),
             static_cast<unsigned long>(preview_no_new_frame.load()),
             static_cast<unsigned long>(preview_too_old.load()),
             static_cast<unsigned long>(preview_decode_failed.load()),
             static_cast<unsigned long>(preview_blanked.load()),
             static_cast<unsigned long>(edge_jpeg_lock_timeouts.load()),
             static_cast<unsigned long>(edge_jpeg_decode_failures.load()),
             static_cast<unsigned long>(preview_borrowed.load()));
    usb_live.store(hal_uvc_is_streaming(), std::memory_order_relaxed);
    presence_text.store(presence, std::memory_order_relaxed);
}
