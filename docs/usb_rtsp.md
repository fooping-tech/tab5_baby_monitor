# USB UVC + H.264 RTSP integration

## Provenance

Reference: [tab5_rtsp_logger e5e5fee](https://github.com/fooping-tech/tab5_rtsp_logger/tree/e5e5fee79c2232bd5de4a994d8f90e12f4630952).
The reference checkout is read-only and its application/UI is not built into this project.

- `firmware/main/hal/hal_uvc.*`: adapted from reference `platforms/tab5/main/hal/components/hal_uvc.cpp`
  and `hal/hal_uvc.h`. Keep UVC negotiation/profile fallback, three-slot copying, reconnect/stall recovery and stop ownership.
  Remove LVGL preview/JPEG render buffers. The remaining reclamation worker only releases ready markers, not pixel conversion.
  Added callback timestamp and stream generation to the copy contract; two consumers never borrow camera-driver buffers.
- `firmware/main/rtsp/*` and `tests/rtsp_framing_test.cpp`: reference RTSP transport, Annex-B, FU-A,
  RTCP separation, session cleanup and deadlines. Removed MIPI encoding/acquisition and implicit MIPI fallback.
  RTSP now checks an application-owned UVC stream and never starts/stops it. Kept the 20KiB PSRAM RTSP task stack.
  Content-Base/RTP-Info use the accepted socket's local IP rather than the reference's hardcoded tab5.local.
- `network.cpp`: reference ESP-Hosted STA netif start/connected/stop hooks, with LAN-only mDNS/RTSP service startup.
  The demo web server, RTC/SNTP and UI dependencies are not imported. Disconnect invokes netif-down and bounded reconnect attempts.
- `board.cpp`: reference BSP I2C pins 31/32 and PI4IOE at 0x44, preserving the WLAN/USB 5V register sequence,
  followed by ESP-IDF USB host event task. No display, speaker, internal sensor or charge-control UI is initialized.
- External reference components: esp_video 0.7.0, esp_cam_sensor 0.7.1, esp_sccb_intf (all pinned by reference SHA).
  Only the H.264 video device is enabled; MIPI/DVP/ISP devices are disabled. No reference source changes are required.

Reference MIT notice is preserved in [licenses/tab5_rtsp_logger-MIT.txt](../licenses/tab5_rtsp_logger-MIT.txt).
Original SPDX notices remain in imported sources. The complete application remains AGPL-3.0-only;
this does not relicense upstream components or imply legal separation from linked code.

## Dataflow and limits

USB request 640x480 @15fps, MJPEG preferred then YUY2, application-lifetime capture.
Callbacks copy at most 2MiB into one of three PSRAM slots; busy slots cause drops, never an unbounded queue.
AI and RTSP each own their copies. AI validates reported/JPEG dimensions, requires input age <=1s,
and uses a 32KiB PSRAM worker stack, priority 2, with a configured delay after every poll/inference (default 1s).
The status loop expires evidence at 10s and immediately resets on observed disconnect or a different stream generation.
Posture remains unknown. The observer and worker share only the short state mutex, never the inference work.

RTSP supports one active client, H.264 payload type 96, TCP interleaved RTP/RTCP on port 8554, path `/baby`.
It retains upstream bounded media-send and encoder recovery behavior. No UDP RTP, authentication, TLS or multi-client fanout.
Supported inference/RTSP dimensions must fit the configured capture size; not every negotiated fallback format is usable.
Use a camera advertising the exact configured resolution; nominal capture/RTSP FPS is not a measured throughput guarantee.
YOLO26 inference, software JPEG decode and RTSP conversion compete for P4 cores, memory bandwidth and PSRAM.
Memory/stack exhaustion or watchdog abort inside upstream code remains possible until hardware testing.

## Safe setup

1. Enable USB input in `idf.py menuconfig` (default). Leave RTSP off for local-only inference.
2. For RTSP, set a private LAN SSID/password and enable the explicit RTSP switch. No image upload or external service is used.
3. Build, then verify the exact Tab5 port, board and power source before manually approving a flash.
   Connect a supported camera to USB-A; use stable power suitable for Tab5 and the camera.
4. Serial output should show UVC negotiated/streaming, AI sequence/score/latency, and presence/status.
   With RTSP enabled, confirm DHCP IP and use `rtsp://tab5-edge-ai.local:8554/baby` (or logged IP).
5. Use TCP transport; do not expose port 8554 publicly. Credentials in sdkconfig and binaries are private.
6. Existing flash layouts need careful review because this build includes an NVS partition. Do not erase NVS automatically.

## Acceptance checklist (hardware pending)

- UVC enumeration and continuous genuinely new sequences; MJPEG and YUY2 separately with rotation/color check.
- AI presence transitions with known attended scenes, no-person negatives, unplug/reset and replug during long inference.
- RTSP OPTIONS/DESCRIBE/SETUP/PLAY, SPS/PPS/IDR and decoded H.264 on LAN; client disconnect must not stop AI.
- Reconnect/slow client/RTCP traffic and camera unplug during PLAY; no stale AI result from an earlier generation.
- Measure inference + stream latency/FPS, minimum internal/PSRAM free blocks, task stack HWM and dropped frames.
- Simultaneous AI+RTSP attended 1-hour soak, then extended soak. Scrypted/HomeKit require separate validation.

Automated builds and framing tests do not complete these hardware gates. See PLANS.md for observed evidence.
