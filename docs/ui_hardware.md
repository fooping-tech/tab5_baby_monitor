# Camera Clock and hardware acceptance

The native portrait display is 720x1280. The top 720x640 area displays
aspect-fitted USB images; the lower dark/cyan panel displays local time and
person-presence status. Posture remains unknown. A disconnected or stale
camera clears the preview. The preview is an independent, approximately
5 Hz consumer and never borrows UVC buffers across decoding.

Layout/colors and the generated camera_clock_font_64.c and
camera_clock_time_88.c fonts are ported from tab5_rtsp_logger revision
e5e5fee79c2232bd5de4a994d8f90e12f4630952, app/apps/app_baby_logger.
The upstream MIT notice is retained in licenses/tab5_rtsp_logger-MIT.txt.
This does not import the full launcher, storage or Home Assistant application.
The pinned upstream BSP handles LCD identification, touch and both IO expanders.

Time is synchronized by SNTP when the RTSP network is enabled, using JST.
Until synchronization the display explicitly indicates an unsynchronized clock;
offline RTC initialization is not yet ported.

## Evidence gates

1. Host tests and IDF build: compilation is not hardware proof.
2. Identify exact chip/MAC/port and privately back up flash before replacement.
3. Record application SHA256, flash verification and boot/reset serial output.
4. Confirm physical display, colors and orientation with an attended observation.
5. Check fresh USB frames and independent AI, preview and RTSP operation.
6. Decode RTSP over TCP without recording private imagery; reconnect once.
7. Run at least one real elapsed hour while collecting serial and decoder logs.
   Record decoded frames, errors, resets, heap trend and test conditions.

Keep flash backups, Wi-Fi configuration, serial logs and any camera imagery
outside Git. A successful soak does not establish baby-detection accuracy or
sleep safety. Scrypted/HomeKit and intentional watchdog fault injection remain
separate acceptance tests.
