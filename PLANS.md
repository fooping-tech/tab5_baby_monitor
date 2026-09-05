# Tab5 Baby Edge AI — implementation ledger

## Scope / acceptance
- AGPL-3.0-only research PoC, separate from tab5_rtsp_logger; no changes to that repository.
- Implement ESP32-P4 stock YOLO26n person inference on an explicitly supplied still JPEG, plus a portable presence state machine and host tests.
- Expose an RGB frame boundary for a future Tab5 camera adapter; do not pretend still-image inference is live camera validation.
- Document private data collection, baby one-class training, ESP-PPQ conversion, model provenance and evaluation.
- Person is not baby identity. Presence is not breathing, sleep safety, or a medical alarm. Posture stays unknown until a separate validated classifier exists.
- Verify host tests and, if environment permits, ESP-IDF compile separately from hardware, accuracy, camera, RTSP and soak acceptance.

## Plan
1. Inspect empty remote and upstream APIs; pin dependencies.
2. Bootstrap main with this ledger and AGPL license, then create an implementation PR branch (empty remote has no PR base).
3. Add firmware, state tests, evaluation tooling, dataset examples and documentation.
4. Run available automated checks, record blockers and publish PR; do not merge or flash hardware.

## Evidence / status
- 2026-09-05: remote is empty, no AGENTS.md in checkout or workspace parents.
- Upstream ESP-DL inspected: 5d9c36063dddbe98b5387828c831d6bbadb1370f, YOLO26 component 0.1.0.
- Implemented: stock person still-image runner, synchronous RGB888 adapter, fail-to-unknown presence filter, host tests, CSV evaluator, model/data templates and pipeline/architecture/evaluation/license documentation.
- Host: CMake + CTest 1/1 passed; Python unittest 6/6 passed; synthetic CSV evaluator passed. These are not model accuracy evidence.
- ESP-IDF v5.4.2 / ESP32-P4 build passed with official upstream bus.jpg as the build fixture, before final USB-console default verification. No images or binaries are committed.
- Initial sandbox build failed at psutil process enumeration; the identical build succeeded with approved execution. Dependencies resolved: esp_new_jpeg 1.0.2, dl_fft 0.6.0; now explicitly pinned.
- Stock model SHA256 checked at build: 283e4a2ca8626bf34ff4da290597afcc8e5eaa50b939ec589961806e3d9da99a.
- Manual upstream conversion caveat documented: calibration square Resize differs from device letterbox; adapt private preprocessing before accepting a custom model.

## Remaining boundaries
- Final clean-default build: passed with a newly generated `build/sdkconfig.verify`, USB Serial/JTAG and both watchdogs enabled. ESP-IDF v5.4.2 / P4; image fixture is upstream bus.jpg, compilation only.
- App size 6,304,160 bytes (60% of app partition free); SHA256 ebef6b33ec8ae8dde90a9ea542eea8662dfcafc81762ea2a7004411ddc89fce1. This is a local pre-commit build artifact, not a flashed device or release binary.
- Staged whitespace check passed; staged files contain no images, recordings or model binaries. PR/remote CI status will be reported on GitHub and at handoff.
- No device flash, on-device inference, live camera adapter, rotation/ROI proof, RTSP/UI integration, reconnect or soak performed.
- No child data collected/uploaded, baby training, ESP-PPQ execution, quantitative accuracy evaluation or posture implementation performed.
- Static single-image inference intentionally leaves temporal presence unknown; fresh live frames are required for dwell-based presence.
- Thresholds and latency budgets need attended application-specific evaluation. This is not a safety or medical monitor.
