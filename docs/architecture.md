# Architecture and extension gates

## Current path

Default: USB UVC → bounded 3-slot compressed/raw copy broker → independent RTSP and AI consumers.
AI copies the newest frame, decodes MJPEG in software (or converts YUY2), then runs PersonDetector → PresenceFilter.
RTSP uses its own copy, hardware JPEG conversion / hardware H.264 V4L2 and TCP interleaved RTP.
Optional still mode: consented JPEG → RGB888 → PersonDetector → PresenceFilter → one serial log.
PersonDetector owns dl::Model before YOLO26, so destruction releases the processor before its model.
The upstream preprocessor owns letterboxing (114), RGB normalization (0/255) and tensor quantization;
do not resize twice or add NMS to the one2one decoder. Max 32 detections, upstream decode threshold 0.25,
presence threshold 0.5. Crowded scenes may truncate a person outside top-K; measure this limitation.

## Live Tab5 camera adapter (implemented, hardware unverified)

Supply a packed, contiguous RGB888 `RgbFrame` with `bytes = width * height * 3`.
`captured_ms` and `now_ms` must use the same monotonic clock (esp_timer / 1000).
Pixels are borrowed only during synchronous `detect`; camera owner must keep them stable and release afterwards.
Convert RGB565/JPEG/strided DMA buffers in the adapter, with explicit length and ownership checks.
No inference or conversions on a UVC callback, LVGL lock, RTSP task or interrupt.
The application uses a single priority-2 inference worker and a bounded latest-frame broker; discard superseded frames,
never queue an unbounded backlog. Do not share the non-thread-safe detector across tasks.

Each newly captured frame calls `filter.update(captured_ms, now_ms, result.valid, result.person_score)`.
Duplicate/out-of-order timestamps, invalid scores, decode failure, incompatible model and excessive age yield unknown.
The main loop calls `filter.current(now_ms)` every second even when the source disconnects;
otherwise a cached present value can survive a blocked inference worker. Do not refresh timestamps on replayed frames.
Log capture time, completion time, sequence, validity/reason, score, state, model hash and config version.
Hardware aborts/asserts inside upstream allocation/model execution cannot be converted into a result by this scaffold:
external stale handling and reboot-to-unknown are necessary; measure OOM and watchdog behavior on-device.

Initial integration should crop a configured crib ROI before inference and report ROI-relative meaning, not whole-room identity.
Confirm portrait rotation/mirror and aspect ratio with annotated test images before using ROI logic.
Both runners currently use the entire image and have no ROI. Acquisition timestamp comes from the USB callback,
not inference start. Stream generations prevent pre-disconnect inference from being published after reconnect.
The application owns UVC for its whole lifetime; RTSP TEARDOWN cannot stop AI capture. No MIPI fallback is permitted.

## Baby one-class detector

Keep an independent versioned detector adapter rather than silently changing COCO class 0 to baby.
Current firmware intentionally rejects class count != 80 and selects only the pinned stock model.
For a future baby model, explicitly change CMake model selection, expected input dimensions, head shapes,
label count/name, target class and provenance manifest together; require held-out evaluation before enabling it.
Share `PresenceFilter`, not model-specific decoder assumptions. Person false positives from caregivers must be measured.

## Posture stage

Future contract: baby ROI → classifier → `{supine, not_supine, unknown}` with confidence, quality,
capture timestamp, model hash and rejection reason. No classification if missing baby, occlusion, blur,
multiple ambiguous boxes, out-of-distribution input, stale crop or insufficient confidence.
Unknown is not supine. Define not_supine precisely in the annotation guide (e.g. prone or lateral),
and retain separate subclass annotations for audit. Do not infer posture from the person bounding-box aspect ratio.
The enum is only a contract: no posture model, medical conclusion or alarm exists in this implementation.
