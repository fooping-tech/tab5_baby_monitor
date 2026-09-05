/*
 * SPDX-FileCopyrightText: 2026 OpenAI
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Initializes the UVC client on top of the USB Host library installed by the
// Tab5 BSP.  This function must be called after bsp_usb_host_start().
bool hal_uvc_init(void);

// Indicates that the UVC component is initialized.  A true result means the
// camera can be selected; it does not claim that a physical camera is present.
bool hal_uvc_is_available(void);

typedef enum {
    HAL_UVC_UNINITIALIZED = 0,
    HAL_UVC_DRIVER_READY,
    HAL_UVC_DEVICE_ABSENT,
    HAL_UVC_DEVICE_PRESENT,
    HAL_UVC_NEGOTIATING,
    HAL_UVC_STREAMING,
    HAL_UVC_DISCONNECTED,
    HAL_UVC_ERROR,
} hal_uvc_state_t;

hal_uvc_state_t hal_uvc_state(void);
bool hal_uvc_is_connected(void);
bool hal_uvc_is_streaming(void);
const char *hal_uvc_name(void);

// Read-only UVC broker health. Each ownership mask uses one bit per internal
// slot (bit 0 is slot 0). The copy takes only the short metadata mutex and
// never borrows or exposes a frame buffer.
typedef struct {
    hal_uvc_state_t state;
    bool initialized;
    bool connected;
    bool capturing;
    bool streaming;
    bool stop_pending;
    bool restart_blocked;
    uint32_t frames;
    uint32_t drops;
    uint32_t active_copies;
    uint32_t callback_max_us;
    uint32_t callback_slow;
    uint32_t preview_frames;
    uint32_t preview_decode_errors;
    uint32_t watchdog_restarts;
    uint32_t slot_writing_mask;
    uint32_t slot_ready_mask;
    uint32_t slot_rendering_mask;
    uint32_t slot_copying_mask;
    uint32_t slot_valid_mask;
    uint32_t stream_task_stack_hwm;
    uint32_t render_task_stack_hwm;
} hal_uvc_metrics_t;

bool hal_uvc_get_metrics(hal_uvc_metrics_t *metrics);

typedef enum {
    HAL_UVC_FRAME_UNKNOWN = 0,
    HAL_UVC_FRAME_MJPEG = 1,
    HAL_UVC_FRAME_YUY2 = 2,
} hal_uvc_frame_format_t;

typedef struct {
    size_t bytes;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t sequence;
    int64_t captured_us;
    uint32_t generation;
    hal_uvc_frame_format_t format;
} hal_uvc_frame_info_t;

// Copy the newest UVC payload without borrowing an internal slot.  Preview
// and RTSP can therefore consume the same camera stream independently; the
// caller owns dst and may reuse it after the function returns.
bool hal_uvc_copy_latest_frame(uint32_t last_sequence, uint8_t *dst, size_t capacity,
                               hal_uvc_frame_info_t *info);

bool hal_uvc_start(void);
uint32_t hal_uvc_generation(void);
void hal_uvc_stop(void);
bool hal_uvc_is_capturing(void);

#ifdef __cplusplus
}
#endif
