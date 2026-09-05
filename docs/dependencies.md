# 依存関係とライセンス記録

2026-09-05に確認しました。依存コンポーネントはvendorとして取り込まず、取得時に固定します。
USB/RTSPアプリケーションソースは来歴を記録して移植し、MIT通知を保持します。詳細は [usb_rtsp.md](usb_rtsp.md) を参照してください。

| 依存関係 | 選定 | ライセンス / 役割 |
|---|---|---|
| ESP-IDF | v5.4.2 | Apache-2.0および各コンポーネント通知、ESP32-P4 SDK |
| ESP-DL | Git `5d9c36063dddbe98b5387828c831d6bbadb1370f`（component 3.3.11） | MIT、推論ランタイム |
| ESP-DL YOLO26 wrapper | 同revision（component 0.1.0） | 上流はMIT。wrapperはYOLO重みを再ライセンスしない |
| YOLO26n標準事前学習重み | 同revisionの `models/yolo26/models/p4/yolo26n_512_s8_p4.espdl` | Ultralytics由来としてAGPL-3.0扱い |
| Ultralytics | 8.4.7 | AGPL-3.0、学習・量子化チュートリアル互換 |
| ESP-PPQ | 上流requirementsの1.2.4 | 上流ライセンス/通知を保持、量子化専用 |
| tab5_rtsp_logger参照 | `e5e5fee79c2232bd5de4a994d8f90e12f4630952` | MIT、UVC/RTSP/映像部品。通知を保持 |
| usb_host_uvc / esp_h264 | 2.5.1 / 1.0.4 | 上流通知、参照統合の版に固定 |
| esp_hosted / esp_wifi_remote / mdns | 1.4.0 / 0.8.5 / 1.8.2 | 上流通知、SDIO Wi-Fiとローカル検出 |
| LVGL / ESP LVGL port | 9.2.2 / 参照component 2.5.0 | MIT / Apache-2.0、縦画面表示 |
| Tab5 BSP / ST7121 driver | 固定した参照revision | 上流Apache-2.0通知を保持 |

上流の量子化requirementsは `onnx==1.17.0`、`onnxruntime>=1.19.0`、`torch>=2.4.0`、
`torchvision>=0.19.0`、`numpy<2.0.0`、`onnxsim>=0.4.36` も指定します。これは完全な再現性lockではありません。
モデル採用前にpip freeze、Python/OS/CUDA版、編集済みノートブックを私有領域へ保存します。

ファームウェア依存の `esp_new_jpeg 1.0.2` と `dl_fft 0.6.0` は `main/idf_component.yml` で固定します。
生成される `dependencies.lock` は端末固有の外部ESP-DLパスを含むためGit管理せず、ビルド証跡とともに私有保管します。
CMakeはソースrevisionと標準モデルSHA256を検査します。標準モデルSHA256は
`283e4a2ca8626bf34ff4da290597afcc8e5eaa50b939ec589961806e3d9da99a` です。

データセット、baby/posture学習重みは提供しません。モデル再配布と学習データの権利は別に確認してください。
コードをAGPLにしても画像の公開権利は得られません。必要なソース、ビルドスクリプト、変更内容、第三者通知を保持し、
ネットワーク提供や結合成果物の義務を配布前に確認します。これはプロジェクト方針であり法律相談ではありません。

参照: [ESP-DL固定revision](https://github.com/espressif/esp-dl/tree/5d9c36063dddbe98b5387828c831d6bbadb1370f)、
[Ultralyticsライセンス](https://www.ultralytics.com/license)、[AGPL-3.0](https://www.gnu.org/licenses/agpl-3.0.html)。
