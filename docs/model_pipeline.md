# Private data → training → quantization → ESP-DL

## 1. Collection and annotation

- Obtain guardian consent and rights for all visible people; restrict access, encrypt storage and define deletion date.
- Capture locally on a consented camera; no cloud upload, Roboflow API key or automatic collection in this repository.
- Save infrequent frames (e.g. 1 frame / 5 seconds) in private `data/`, with session ID, subject pseudonym,
  timestamp, camera/ROI/rotation, illumination, distance, occlusion and consent record reference in a local manifest.
- Collect normal attended scenes only; never arrange unsafe infant positions for data collection.
- Include empty crib, caregiver-only, toys/dolls, blankets, dim lighting, blur and partial visibility as negatives/challenges.
- Annotate YOLO boxes as `class_id center_x center_y width height`, normalized to [0,1]. Baby is class 0;
  empty negative frames have an empty label file. Human-review box bounds, image pairing and ambiguous cases.
- Split by child/household/session **before** frame extraction or augmentation, not random adjacent frames.
  Suggested starting split 70/15/15 by groups; enforce group disjointness and record SHA256 of each image and split list.
- Calibration uses a representative subset of train only. Keep val for tuning and test untouched until final evaluation.
- Store posture labels separately from detector boxes: supine/not_supine/unknown, with uncertainty and visibility reasons.
- Never commit private images, videos, raw logs, model weights trained on private data, or embedded-image firmware binaries.

Layout under private dataset root:

```text
images/{train,val,test}/<session>_<frame>.jpg
labels/{train,val,test}/<session>_<frame>.txt
```

Copy `configs/baby.example.yaml` into private storage and edit its absolute dataset root. Keep consent metadata separate.

## 2. Train a future baby detector (not executed here)

Use a separate Python 3.10/3.11 environment; available RAM/GPU determines batch size. Review dependencies before installation.

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements-training.txt
yolo detect train model=yolo26n.pt data=/absolute/private/baby.yaml imgsz=512 epochs=100 batch=8 seed=42 project=artifacts/train name=baby_v1
yolo detect val model=artifacts/train/baby_v1/weights/best.pt data=/absolute/private/baby.yaml split=test imgsz=512
pip freeze > artifacts/training-environment.txt
```

The first command may download the upstream pretrained model. Record its source URL and SHA256 before training.
These epochs/batch settings are starting points, not validated hyperparameters. Save config, seed, split hash,
training code revision, model hashes, val/test metrics and failure examples. Evaluate baby-only and caregiver-only scenes separately.

## 3. Quantize and convert (manual upstream notebook workflow)

Stock person PoC needs **no** training or conversion: use the pinned zoo `.espdl` file.
For retrained weights use the **pinned** ESP-DL tutorial at:
`$ESP_DL_PATH/examples/tutorial/how_to_quantize_model/quantize_yolo26/`.
Do not use a generic `yolo export format=onnx` result as if it were compatible with this firmware.
YOLO26 decoder expects six named box/class heads, not the generic postprocessed tensor.

```sh
cd "$ESP_DL_PATH/examples/tutorial/how_to_quantize_model/quantize_yolo26"
python3 -m venv /absolute/private/quant-env
. /absolute/private/quant-env/bin/activate
pip install -r requirements.txt
pip install jupyterlab
jupyter lab quantize_yolo26_coco.ipynb
```

Make a private working copy of the notebook **in the same directory** (relative `scripts/` and `esp_ppq_lut/` imports).
Review cells before running: disable any dataset downloading/fallback, and do not use the Roboflow upload/download workflow.
In the COCO notebook set `IMG_SZ_I=512`, `PLATFORM="p4"`, `DATA_YAML_FILE_I` to the local baby YAML.
In `QATConfig`, set `PT_FILE` to the trained best.pt, `DATA_FALLBACK_PATH` to private train images,
and `ESPDL_OUTPUT_DIR` to a private explicit path; tune calibration batch/steps to memory.
Check the loader uses the train split, derives one class, and does not retain COCO labels/evaluation settings.
The pinned `scripts/dataset.py` calibration loader uses a square Resize, not the device's letterbox.
For non-square frames, adapt the private loader to RGB letterbox with padding 114 and matching interpolation,
or point calibration at a private train-only directory of already letterboxed 512x512 RGB images.
Save that preprocessing change and compare its tensors against device/emulator preprocessing before accepting calibration.
The loader lists only the selected directory (not nested sessions) and truncates unsorted file order;
provide a flat, deterministically selected representative subset and record its ordered file list/hash.
Keep `INT16_LUT_STEP_I=32` initially. Save the edited notebook/config and Python lock with the result.

Run upstream stages sequentially: patched one2one ONNX export, calibration, TQT scale refinement,
passive/alignment passes, mixed INT8/INT16 LUT fusion, six-head graph surgery, `.espdl` export.
Inspect output names `one2one_p3_box`, `one2one_p3_cls` through p5, 4 box channels and 1 baby class channel,
batch 1 NHWC shapes and matching INT8/INT16 dtype per box/class pair. For stock COCO class channels are 80.
Record the actual input shape, output names, scales, normalization and target in a copy of the model manifest template.

Run the notebook emulator and compare identical private held-out images before/after quantization,
then compare on-device boxes/scores on the same images. Check letterbox padding, RGB order and inverse coordinate mapping.
Set a project-specific acceptable recall/mAP drop **before** tuning; do not claim conversion success proves task accuracy.
This repository does not yet automate/validate the notebook or deploy a baby model. Deployment requires the explicit
adapter changes in [architecture](architecture.md), not simply renaming a baby file as the COCO model.

Upstream reference: [YOLO26 conversion tutorial at pinned revision](https://github.com/espressif/esp-dl/tree/5d9c36063dddbe98b5387828c831d6bbadb1370f/examples/tutorial/how_to_quantize_model/quantize_yolo26).
