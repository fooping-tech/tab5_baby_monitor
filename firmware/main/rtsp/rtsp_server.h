/*
 * SPDX-License-Identifier: MIT
 *
 * Minimal RTSP/H.264 server entry point for the Tab5.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "hal/hal_uvc.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the TCP RTSP listener. Camera hardware is opened on the first RTSP session. */
esp_err_t rtsp_server_start(void);
esp_err_t rtsp_server_prepare(void);

/**
 * Borrow the most recently decoded USB frame instead of decoding it again.
 *
 * The RTSP pipeline already runs every frame it streams through the one
 * hardware JPEG engine. The preview used to decode the same frames a second
 * time on that same engine, which saturates near 17 decodes per second at
 * 640x480 and left the preview's decode call blocking for 133-195 ms against
 * its 200 ms period.
 *
 * Returns false when no frame newer than `last_sequence` has been decoded, or
 * the borrow could not be taken within `timeout_ms`; the caller then decodes
 * for itself, which is cheap because an idle pipeline means an idle engine.
 * The buffer is RGB888 and stays valid only until rtsp_release_decoded_frame().
 * The pipeline cannot decode its next frame while a borrow is open, so copy
 * what you need, release immediately, and never hold it across a blocking
 * call such as bsp_display_lock().
 *
 * Freshness is still the caller's decision: `info.captured_us` is the capture
 * timestamp, and a borrowed frame is no more current than that says.
 */
bool rtsp_borrow_decoded_frame(uint32_t last_sequence, const uint8_t **rgb888,
                               hal_uvc_frame_info_t *info, uint32_t timeout_ms);
void rtsp_release_decoded_frame(void);

// Read-only server/pipeline health. Counters are lifetime values for the
// current boot and are not reset when an RTSP client disconnects.
typedef struct {
    bool server_started;
    bool pipeline_ready;
    bool active_client;
    uint8_t pipeline_state;
    uint8_t video_source;
    uint32_t task_stack_hwm;
    uint64_t connections;
    uint64_t access_units;       // Successfully sent H.264 access units.
    uint64_t packets;            // Successfully sent RTP packets only.
    uint64_t bytes;              // All successfully sent RTSP/RTP/RTCP socket bytes.
    uint64_t send_timeouts;
    uint64_t socket_errors;
    uint64_t encoder_timeouts;
    uint64_t encoder_errors;
    uint64_t recoveries;         // Bounded recovery attempts.
    uint64_t rtp_stall_watchdogs; // RTP no-send watchdog expirations.
} rtsp_metrics_t;

bool rtsp_server_get_metrics(rtsp_metrics_t *metrics);

#ifdef __cplusplus
}
#endif
