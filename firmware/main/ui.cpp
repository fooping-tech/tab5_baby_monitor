#include "live_services.hpp"
#include "bsp/m5stack_tab5.h"
#include "hal/hal_uvc.h"
#include "frame_convert.hpp"
#include "jpeg_decoder.hpp"
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
std::atomic<bool> usb_live{false};
std::atomic<const char *> presence_text{"unknown"};
constexpr size_t payload_capacity = 1024 * 1024;

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
    if (local.tm_year >= 124) {
        char text[64];
        strftime(text, sizeof(text), "%Y/%m/%d", &local);
        lv_label_set_text(date_label, text);
        const char *weekdays[] = {"(SUN)", "(MON)", "(TUE)", "(WED)", "(THU)", "(FRI)", "(SAT)"};
        lv_label_set_text(weekday_label, weekdays[local.tm_wday]);
        strftime(text, sizeof(text), "%H:%M:%S", &local);
        lv_label_set_text(clock_label, text);
    }
    lv_label_set_text_fmt(status_label, "USB // %s\nPERSON // %s\nPOSTURE // unknown",
                          usb_live.load(std::memory_order_relaxed) ? "LIVE" : "OFFLINE",
                          presence_text.load(std::memory_order_relaxed));
}

void preview(void *)
{
    auto *payload = static_cast<uint8_t *>(heap_caps_malloc(payload_capacity, MALLOC_CAP_SPIRAM));
    auto *rgb = edge_jpeg_rgb_buffer_alloc(CONFIG_TAB5_UVC_WIDTH * CONFIG_TAB5_UVC_HEIGHT * 3);
    uint32_t sequence = 0;
    int64_t displayed_us = 0;
    bool has_preview = false;
    while (payload && rgb) {
        hal_uvc_frame_info_t frame{};
        uint8_t *decoded = nullptr;
        if (hal_uvc_copy_latest_frame(sequence, payload, payload_capacity, &frame)) {
            sequence = frame.sequence;
            if (frame.width && frame.height && frame.width <= CONFIG_TAB5_UVC_WIDTH &&
                frame.height <= CONFIG_TAB5_UVC_HEIGHT) {
                if (frame.format == HAL_UVC_FRAME_MJPEG) {
                    if (edge_decode_jpeg_rgb888(payload, frame.bytes, frame.width, frame.height, rgb,
                                                 CONFIG_TAB5_UVC_WIDTH * CONFIG_TAB5_UVC_HEIGHT * 3)) decoded = rgb;
                } else if (frame.format == HAL_UVC_FRAME_YUY2 &&
                           baby_edge::yuy2_to_rgb(payload, frame.bytes, frame.width, frame.height,
                               frame.stride, rgb, CONFIG_TAB5_UVC_WIDTH * CONFIG_TAB5_UVC_HEIGHT * 3)) {
                    decoded = rgb;
                }
            }
        }
        if (decoded && frame.generation == hal_uvc_generation() && hal_uvc_is_streaming() &&
            esp_timer_get_time() - frame.captured_us < 1000000) {
            const int width = 720;
            const int height = std::min(640, static_cast<int>(720 * frame.height / frame.width));
            const int fitted_width = height * frame.width / frame.height;
            memset(render_pixels, 0, 720 * 640 * sizeof(uint16_t));
            const auto *source = static_cast<const uint8_t *>(decoded);
            for (int row = 0; row < height; ++row) {
                for (int column = 0; column < fitted_width; ++column) {
                    const size_t offset = ((row * frame.height / height) * frame.width +
                                           column * frame.width / fitted_width) * 3;
                    render_pixels[(row + (640 - height) / 2) * width + column + (width - fitted_width) / 2] =
                        ((source[offset] >> 3) << 11) | ((source[offset + 1] >> 2) << 5) | (source[offset + 2] >> 3);
                }
            }
            if (bsp_display_lock(0)) {
                memcpy(pixels, render_pixels, 720 * 640 * sizeof(uint16_t));
                lv_obj_invalidate(canvas);
                displayed_us = frame.captured_us;
                ++preview_frames;
                has_preview = true;
                bsp_display_unlock();
                lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
            }
        } else if (has_preview && (!hal_uvc_is_streaming() || esp_timer_get_time() - displayed_us > 1000000)) {
            memset(render_pixels, 0, 720 * 640 * sizeof(uint16_t));
            if (bsp_display_lock(0)) {
                memcpy(pixels, render_pixels, 720 * 640 * sizeof(uint16_t));
                lv_obj_invalidate(canvas);
                has_preview = false;
                bsp_display_unlock();
                lvgl_port_task_wake(LVGL_PORT_EVENT_DISPLAY, nullptr);
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    heap_caps_free(payload);
    heap_caps_free(rgb);
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
    pixels = static_cast<uint16_t *>(heap_caps_calloc(720 * 640, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
    if (!pixels) return ESP_ERR_NO_MEM;
    render_pixels = static_cast<uint16_t *>(heap_caps_calloc(720 * 640, sizeof(uint16_t), MALLOC_CAP_SPIRAM));
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
    date_label = label(root, 724, "----/--/--", 0xB5D7E1);
    lv_obj_set_width(date_label, 720);
    lv_obj_set_style_text_font(date_label, &lv_font_camera_clock_64, 0);
    lv_obj_set_style_text_letter_space(date_label, 2, 0);
    weekday_label = label(root, 800, "-", 0xFF69DD);
    lv_obj_set_width(weekday_label, 720);
    lv_obj_set_style_text_font(weekday_label, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(weekday_label, 2, 0);
    clock_label = label(root, 850, "--:--:--", 0x65F5FF);
    lv_obj_set_pos(clock_label, 8, 850);
    lv_obj_set_width(clock_label, 704);
    lv_obj_set_style_text_font(clock_label, &lv_font_camera_clock_time_88, 0);
    status_label = label(root, 1050, "AI // unknown", 0x65F5FF);
    label(root, 1192, "TAB5 // CAMERA CLOCK - EDGE AI PoC", 0xD83AAE);
    lv_timer_create(update_labels, 1000, nullptr);
    update_labels(nullptr);
    bsp_display_unlock();
    bsp_display_backlight_on();
    return xTaskCreateWithCaps(preview, "edge_preview", 8192, nullptr, 2, nullptr,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT) == pdPASS ? ESP_OK : ESP_ERR_NO_MEM;
}

void edge_ui_status(const char *presence)
{
    ESP_LOGI("edge_ui", "preview_frames=%lu", static_cast<unsigned long>(preview_frames.load()));
    usb_live.store(hal_uvc_is_streaming(), std::memory_order_relaxed);
    presence_text.store(presence, std::memory_order_relaxed);
}
