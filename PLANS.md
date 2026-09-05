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
- Implementation, host tests, ESP-IDF build, quantization and hardware validation pending.
