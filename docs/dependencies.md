# Dependency and license record

Inspected 2026-09-05. Dependencies are downloaded, not vendored in this repository.

| Dependency | Selection | License / role |
|---|---|---|
| ESP-IDF | v5.4.2 | Apache-2.0 plus component notices; ESP32-P4 SDK |
| ESP-DL | Git 5d9c36063dddbe98b5387828c831d6bbadb1370f (component 3.3.11) | MIT; runtime |
| ESP-DL YOLO26 wrapper | same revision (component 0.1.0) | upstream declares MIT; wrapper is not a relicense of YOLO weights |
| YOLO26n pretrained stock weights | same revision, models/yolo26/models/p4/yolo26n_512_s8_p4.espdl | Ultralytics-derived: treat as AGPL-3.0 |
| Ultralytics | 8.4.7 | AGPL-3.0; training, upstream quantization tutorial compatibility |
| ESP-PPQ | 1.2.4 in upstream requirements | retain upstream license/notices; quantization only |

Upstream quantization requirements also specify onnx==1.17.0, onnxruntime>=1.19.0,
torch>=2.4.0, torchvision>=0.19.0, numpy<2.0.0 and onnxsim>=0.4.36.
Those ranges are **not a complete reproducibility lock**. Capture pip freeze, Python/platform/CUDA versions
and the edited notebook before accepting a model. The training-only requirements intentionally do not install the quantizer.
Firmware dependencies esp_new_jpeg 1.0.2 and dl_fft 0.6.0 are pinned in main/idf_component.yml,
along with IDF 5.4.2. The generated dependencies.lock contains a machine-specific external ESP-DL path,
so it is ignored by Git; retain it privately with build evidence. Source revision and model SHA256 are checked by CMake.
Stock model SHA256: `283e4a2ca8626bf34ff4da290597afcc8e5eaa50b939ec589961806e3d9da99a`.

No dataset or learned baby/posture weights are supplied. Model redistribution/training-data rights must be reviewed independently;
do not assume applying AGPL to code grants image rights or makes private data suitable for publication.
Keep corresponding source/build scripts and modifications available as required, preserve third-party notices,
and review network/combined-work obligations before deployment. This is a project policy, not legal advice.

Sources: [ESP-DL revision](https://github.com/espressif/esp-dl/tree/5d9c36063dddbe98b5387828c831d6bbadb1370f),
[Ultralytics licensing](https://www.ultralytics.com/license), [AGPL-3.0](https://www.gnu.org/licenses/agpl-3.0.html).
