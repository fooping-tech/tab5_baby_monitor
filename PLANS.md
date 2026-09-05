# Tab5 Baby Edge AI — implementation ledger

## USB camera / RTSP integration (2026-09-05)
- Reference: tab5_rtsp_logger e5e5fee79c2232bd5de4a994d8f90e12f4630952, read-only reference checkout. Preserve upstream MIT notices in imported sources.
- [x] Port headless UVC negotiation, bounded copy broker and recovery; add capture timestamps and independent AI consumption.
- [x] Reuse H.264 V4L2 / RTSP TCP framing and transport; USB-only (no implicit MIPI fallback), capture lifetime owned by application not RTSP session.
- [x] Add Tab5 USB/WLAN power, USB host, ESP-Hosted SDIO network configuration and opt-in LAN RTSP.
- [x] Run AI independently at bounded cadence, expire stale/invalid results without blocking capture or RTSP; keep still-image mode selectable.
- [x] Reuse presence/timestamp tests, add YUY2 conversion tests and RTSP framing tests, build both modes with IDF 5.4.2, update documentation. UVC generation/concurrency behavior still needs hardware validation.
- No flash, camera hardware, network streaming, accuracy or soak claims from builds. Do not modify reference repository or store credentials/images.

### Integration verification log
- Host CTest 3/3 (presence, imported RTSP framing, new YUY2 RGB/stride/bounds tests) and Python unittest 6/6 passed; staged whitespace check passed.
- USB/RTSP enabled build passed on restored ESP-IDF v5.4.2 / ESP32-P4 with a new build directory. Only a synthetic build-only SSID was used; no credentials or images are committed. SDIO, USB camera, RTSP and watchdog settings checked in generated config.
- During continuation, the earlier /tmp SDK/toolchain had disappeared. Restored official v5.4.2 and P4 tools under work/, completed a failed shallow submodule fetch, and rebuilt from scratch instead of relying only on the earlier binary.
- Upstream warnings remain: esp_video unused sccb_mark with MIPI disabled, and lwIP/Linux ioctl macro redefinitions. Build completed; no upstream dependency edits made.
- Final still-mode rebuild passed with USB mode disabled and upstream bus.jpg as local compile fixture. Both configurations rebuilt against the restored SDK and final source; no flash performed.
- Live/RTSP app: 6,815,456 bytes, 57% app partition free, SHA256 26ae7d00cf1a395f3041cf582e5e153123fcc7f14da9e68dbf36964ac667cf8b. This is a local pre-commit build, not a release or flashed artifact.
- Changes will be committed to the existing open PR #1; remote CI status is reported at handoff.
- Hardware boundary remains open: no flash, USB enumeration, simultaneous live inference/RTSP playback, reconnect, Scrypted/HomeKit, RGB orientation or soak performed. No baby training or posture classification added.

## Initial scaffold scope / acceptance (historical)
- AGPL-3.0-only research PoC, separate from tab5_rtsp_logger; no changes to that repository.
- Implement ESP32-P4 stock YOLO26n person inference on an explicitly supplied still JPEG, plus a portable presence state machine and host tests.
- Expose an RGB frame boundary for a future Tab5 camera adapter; do not pretend still-image inference is live camera validation.
- Document private data collection, baby one-class training, ESP-PPQ conversion, model provenance and evaluation.
- Person is not baby identity. Presence is not breathing, sleep safety, or a medical alarm. Posture stays unknown until a separate validated classifier exists.
- Verify host tests and, if environment permits, ESP-IDF compile separately from hardware, accuracy, camera, RTSP and soak acceptance.

## Initial scaffold plan (historical)
1. Inspect empty remote and upstream APIs; pin dependencies.
2. Bootstrap main with this ledger and AGPL license, then create an implementation PR branch (empty remote has no PR base).
3. Add firmware, state tests, evaluation tooling, dataset examples and documentation.
4. Run available automated checks, record blockers and publish PR; do not merge or flash hardware.

## Initial scaffold evidence / status (historical)
- 2026-09-05: remote is empty, no AGENTS.md in checkout or workspace parents.
- Upstream ESP-DL inspected: 5d9c36063dddbe98b5387828c831d6bbadb1370f, YOLO26 component 0.1.0.
- Implemented: stock person still-image runner, synchronous RGB888 adapter, fail-to-unknown presence filter, host tests, CSV evaluator, model/data templates and pipeline/architecture/evaluation/license documentation.
- Host: CMake + CTest 1/1 passed; Python unittest 6/6 passed; synthetic CSV evaluator passed. These are not model accuracy evidence.
- ESP-IDF v5.4.2 / ESP32-P4 build passed with official upstream bus.jpg as the build fixture, before final USB-console default verification. No images or binaries are committed.
- Initial sandbox build failed at psutil process enumeration; the identical build succeeded with approved execution. Dependencies resolved: esp_new_jpeg 1.0.2, dl_fft 0.6.0; now explicitly pinned.
- Stock model SHA256 checked at build: 283e4a2ca8626bf34ff4da290597afcc8e5eaa50b939ec589961806e3d9da99a.
- Manual upstream conversion caveat documented: calibration square Resize differs from device letterbox; adapt private preprocessing before accepting a custom model.

## Initial scaffold remaining boundaries (historical)
- Final clean-default build: passed with a newly generated `build/sdkconfig.verify`, USB Serial/JTAG and both watchdogs enabled. ESP-IDF v5.4.2 / P4; image fixture is upstream bus.jpg, compilation only.
- App size 6,304,160 bytes (60% of app partition free); SHA256 ebef6b33ec8ae8dde90a9ea542eea8662dfcafc81762ea2a7004411ddc89fce1. This is a local pre-commit build artifact, not a flashed device or release binary.
- Staged whitespace check passed; staged files contain no images, recordings or model binaries. PR/remote CI status will be reported on GitHub and at handoff.
- No device flash, on-device inference, live camera adapter, rotation/ROI proof, RTSP/UI integration, reconnect or soak performed.
- No child data collected/uploaded, baby training, ESP-PPQ execution, quantitative accuracy evaluation or posture implementation performed.
- Static single-image inference intentionally leaves temporal presence unknown; fresh live frames are required for dwell-based presence.
- Thresholds and latency budgets need attended application-specific evaluation. This is not a safety or medical monitor.
## Camera Clock UI and attended hardware acceptance (2026-09-05)
- User authorized UI port, flash, USB/RTSP playback and endurance testing.
- Device identified: ESP32-P4 rev v1.3, MAC 80:f1:b2:d1:44:8f, /dev/cu.usbmodem1101. Full 16 MiB flash backup completed privately in work/tab5-before-ui.bin; never publish it.
- Port pinned reference Camera Clock styling/layout and BSP display initialization; independent throttled preview must not hold the UVC broker during decoding or AI inference.
- Validate host tests and IDF build, then flash only the identified device and record binary digest and serial evidence.
- Verify USB frames, AI status, RTSP decoded frames and reconnect independently; ask for physical screen confirmation rather than infer it from logs.
- Run an initial one-hour RTSP decode plus serial-monitor soak; record elapsed time, frames, errors and reset evidence. No recordings or credentials in Git.
- Pending: implementation, build, flash, physical UI confirmation, playback and one-hour soak.
- UI implementation and host tests (CTest 3/3, Python 6/6) complete. Initial UI build and flash digest verification passed (8bddc77b44db530e692fb7a01c21a45f5768002e0e4ed197a3fa97fa8498e59c), but boot exposed a codec legacy/new I2C driver conflict before app_main. This is a failed hardware gate, not working UI.
- Root cause: esp_codec_dev defaults legacy I2C compatibility on; BSP uses the new driver. Disabled compatibility. Also enabled IDF experimental features required by the reference 200 MHz PSRAM setting; without this the requested speed silently resolved to 20 MHz. Rebuilding before further hardware tests.
- Second boot reached LCD/touch and Wi-Fi initialization, then asserted while ESP-DL model loading queried flash mapping from a PSRAM task stack. Address-to-source decoding identified dl::tool::memory_addr_type / fbs::create_fbs_model. Move the AI task stack to internal RAM; retain image/model buffers in PSRAM. Physical UI and streaming gates still open.
- Third boot: corrected binary hash was verified by esptool; display BSP found ST7121, initialized 720x1280 LCD/touch, PSRAM runs at 200 MHz, Wi-Fi received 192.168.2.167 and SNTP returned ESP_OK. No UVC device-connected event or frame arrived; usb=0 and preview_frames=0 are accurate. RTSP listener started but cannot emit a valid stream without UVC frames. Need attended USB-host cable/power/device check before camera, RTSP decode, reconnect and one-hour live soak.
- USB physical probe then confirmed a device enumerates at address 1 before the UVC driver is registered: board/display initialization delayed hal_uvc_init and it missed the one-shot connection event. Initialize and request UVC before the display so a boot-connected camera is accepted; then verify UVC profiles, frames, preview and RTSP separately.
- UVC order fix verified: address 1 profiles are available and a 640x480@15 MJPEG stream produces preview frames and live AI input. RTSP DESCRIBE reached the H.264 driver but VIDIOC_REQBUFS returned ENOMEM with only a 64 KiB largest internal block. AI task high-water mark showed approximately 30 KiB unused from its 32 KiB internal stack; reduce it to 12 KiB before retesting H.264 allocation.
- H.264 preallocation before AI succeeded with a 180 KiB contiguous internal block, but then YOLO model context allocation failed because full-screen double LVGL buffers left less than 2.56 MiB contiguous PSRAM. Use a 120-line partial-display buffer instead of full-screen double buffers, and preflight PSRAM before model construction so an allocation failure stays unknown rather than panicking.
- Partial display buffering allowed H.264 and the 640x480 camera to run together. A 20-second host TCP RTSP decode completed; device logs showed DESCRIBE/SETUP/PLAY, parameter sets, H.264 access units and 3,976,846 bytes sent before expected peer close. AI model context then loaded but its 2 MiB payload plus RGB copy allocation failed safely. The connected camera advertises a 614,400-byte frame, so reduce UI and AI copy buffers to 1 MiB before retesting all three workloads.
- With 1 MiB copies, USB preview, 640x480 H.264 and YOLO inference run concurrently without a reset. A later RTSP reconnect exposed that rtsp_task always called shutdown_video after a normal client close; it then could not reallocate H.264 after YOLO used memory. Retain the prepared encoder across normal disconnects; runtime failures retain their bounded recovery/shutdown.
- A retained-encoder reconnect test completed two successive TCP RTSP sessions. Before the endurance run, serial evidence showed the ESP-DL software JPEG helper trying to allocate a fresh 640x480 RGB buffer for every preview and inference frame; that allocation later failed under concurrent H.264/AI load. Replace it with one shared hardware JPEG decoder and caller-owned fixed RGB buffers, then rebuild and repeat the attended camera, RTSP, reconnect and one-hour gates.
- Fixed-buffer hardware JPEG decoding was rebuilt and flashed. The first allocation needed cache-line alignment; after correcting that, USB is present, preview_frames advanced from 17 to 102 in 24 seconds, and the AI path completed valid inference calls at about 2.3–2.5 seconds. A 30-second RTSP TCP client issued DESCRIBE/SETUP/PLAY and received the prepared H.264 IDR while preview/AI counters continued. The UVC driver did report occasional malformed/missed MJPEG frames; the decoder drops those frames safely, so an attended physical display check and the one-hour error-rate/soak gate remain open.
- Scrypted was configured for `rtsp://tab5.local:8554/baby`, but the flashed image advertised `tab5-edge-ai.local`. A direct 12-second TCP RTSP decode against the DHCP address succeeded, isolating this as an mDNS hostname mismatch rather than an unavailable stream. Change the default to `tab5`, rebuild/flash, then test the exact `tab5.local` URL before returning to Scrypted.
- The rebuilt image now logs `RTSP server started: rtsp://tab5.local:8554/baby` at boot. The RTSP implementation supports TCP interleaving; configure Scrypted for TCP rather than UDP. A subsequent attached decode triggered task-watchdog warnings while the RTSP and AI paths each held CPU 0 during malformed MJPEG decode timeouts. The device continued operating, but this is a failed endurance condition: bound/defer the JPEG work before attempting the one-hour soak.
- User reports delayed second updates and irregular screen refresh. Root cause in source: the clock/date labels are updated only by the live capture/AI control loop, which is delayed by inference and camera work. Move all label changes to LVGL's own one-second timer; pass presence/USB state through atomics so the live loop does no display locking.
- Add an operator-oriented Japanese guide covering private dataset preparation, YOLO26 baby one-class training, held-out evaluation, ESP-DL quantization/conversion, firmware adapter changes, and separate device validation. It must state that the current flashed model remains stock COCO person and custom weights are not deployed merely by training them.
- Added `docs/training_and_inference.md` and linked it from README. It documents the private-only workflow, reproducibility records, split discipline, training/evaluation commands, ESP-DL conversion constraints, required firmware contract changes, and separate hardware gates. `git diff --check`, host CTest 3/3, and Python unittest 6/6 passed; no data collection, training, conversion, custom-model deployment, flash, or accuracy claim was performed for this documentation change.
- User reports a cyan/intermediate frame before the normal image after the display has run for a while. Source comparison with the working reference shows the cause: this UI writes its RGB565 conversion directly into the LVGL canvas buffer, which can still be consumed by asynchronous DSI flushing. Convert into a separate PSRAM render buffer, then copy a stable snapshot under the LVGL lock and wake the display task; drop a preview frame rather than contend with a flush.
- Implemented and flashed the canvas snapshot path. Host CTest 3/3, Python unittest 6/6, whitespace check, and ESP-IDF P4 build passed. A short boot log recorded USB live input, preview frames increasing (17 to 49), valid AI calls, and about 2.34 MiB free PSRAM after the second display buffer. The same captured serial stream includes a task-watchdog event followed by a software CPU reset from the pre-existing CPU-bound JPEG/AI path; therefore it does not establish long-run stability. Physical confirmation that the cyan intermediate frame is gone remains required.
- Translate the user-facing Markdown documentation into Japanese and introduce a beginner Get Started path: model creation, host tests, and separately approved hardware tests. Keep exact commands, privacy restrictions, custom-model adapter boundary, and the unclosed watchdog/endurance conditions explicit.
- Rewrote the README as a Japanese Get Started entrypoint and translated architecture, dependency/license, evaluation, model-pipeline, USB/RTSP, and Camera Clock hardware documents. The three-step introduction separates host tests, optional private baby-model creation, and manually approved Tab5 tests, while linking detailed Japanese guides. Markdown paths and headings were checked; `git diff --check`, host CTest 3/3, and Python unittest 6/6 passed. No model, device, or runtime claim changed.
- User observed mojibake on the display. The imported generated Camera Clock fonts do work in the pinned reference; this project missed reference LVGL settings LV_FONT_FMT_TXT_LARGE and LV_USE_FONT_COMPRESSED. Restore the original font assets and enable those settings before the next flash.
