# Evaluation and acceptance

## Automated checks

Host CTest exercises dwell boundaries, hysteresis, errors, NaN/range rejection, stale input,
duplicate timestamps, gaps, clock rollback and invalid configuration. Python tests check the report math and invalid input.
Neither proves the detector works. The CSV fixture is explicitly synthetic, not measured accuracy.

`python3 tools/evaluate_presence.py /private/evaluation.csv` expects:

```csv
session_id,timestamp_ms,truth,prediction,pipeline_ms
synthetic,0,present,unknown,2100
```

Truth is independently labelled present/absent **for the intended target and ROI**; prediction is present/absent/unknown.
For baby-use evaluation, adults must not count as true baby presence even though COCO person will detect them.
Record every scheduled observation including failures, not only positive or successful detections.
The report gives a full confusion matrix, precision, recall counting unknown as a miss, unknown rate, all-frame accuracy
and nearest-rank p50/p95 pipeline time. Undefined ratios are null, not zero. This is frame-weighted, not duration-weighted;
record gaps and separately compute time/event metrics for irregular sampling. It does not run inference or parse serial logs.

## Model quality (pending)

- Freeze subject/session-disjoint test set, thresholds and model manifest before final evaluation.
- Detection: mAP50 and mAP50-95, person/baby precision/recall, box quality, caregiver/doll false positives.
- Presence: positive recall, false-absent duration, false-present events/hour, entry/exit delay p50/p95, unknown coverage.
- Stratify by light/IR, motion, occlusion, camera angle, distance, subject and blankets. Do not aggregate away hard cases.
- Compare FP32, quantized emulation and device outputs on identical inputs. Preserve private failed examples for human review.
- Future posture: 3-way confusion, per-class precision/recall, rejection coverage, calibration and subgroup failures.
  Do not claim safety from posture classification results.

## Tab5 runtime gate (all pending until separately recorded)

1. Confirm board revision/flash/PSRAM, exact model hash, IDF version, firmware hash, power source and JPEG rights.
2. Build and size-check; manually approve flash; save flash hash verification separately.
3. Static JPEG smoke: decode works, valid=1, plausible person score; no assertions. Single-image presence=unknown is expected.
4. Validate the implemented USB camera adapter: RGB/rotation, genuinely new timestamps and independent AI/RTSP buffer release. ROI remains unimplemented.
5. Measure decode/preprocess/inference/postprocess/total latency and FPS, internal/PSRAM minimum free heap,
   worker stack high-water mark, frame drops, camera disconnect/reconnect and stale-result behavior.
6. Test UVC unplug, allocation failure and blocked inference; consumer must return unknown rather than false assurance.
7. Validate simultaneous AI/RTSP (implemented but hardware unverified); then attended 1-hour soak, longer run and restart recovery. UI remains out of scope.

Set application-specific latency/accuracy budgets after baseline measurement; no real-time FPS claim is made.
PLANS.md is the source of truth for what was actually run. Do not publish private image-bearing firmware/logs with a PR.
