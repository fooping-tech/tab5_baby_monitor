/*
 * SPDX-License-Identifier: MIT
 *
 * Minimal RTSP/H.264 server entry point for the Tab5.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** Start the TCP RTSP listener. Camera hardware is opened on the first RTSP session. */
esp_err_t rtsp_server_start(void);
esp_err_t rtsp_server_prepare(void);

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
