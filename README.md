# Tab5 Camera Clock

M5Stack Tab5（ESP32-P4）向けのカメラ時計ファームウェアです。USB UVCカメラの映像を本体画面に表示し、
同じ映像をH.264でRTSP配信し、SNTPで同期したローカル時刻を表示します。

以前このリポジトリには人物検出（YOLO26n）が含まれていましたが削除しました。推論は行いません。

## できること

- USB UVCカメラの取得（MJPEG優先、YUY2フォールバック）
- 720x1280縦画面の上部にアスペクト比を保った映像プレビュー、下部にローカル時刻
- H.264 RTSP配信（`rtsp://<host>.local:8554/baby`、TCPインターリーブ）
- SNTPによる時刻同期と定期再同期

RTSPは平文かつ認証なしです。信頼できるLAN内でのみ有効にしてください。インターネットに公開しないでください。

## PC上のテスト

Tab5やカメラがなくても実行できます。

```sh
git clone https://github.com/fooping-tech/tab5_baby_edge_ai_agpl.git
cd tab5_baby_edge_ai_agpl
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
```

RTSPフレーミングと画像変換の自動テストです。カメラ入力、RTSP再生、画面表示、
長時間運転の証明にはなりません。

## Tab5で動かす

Tab5、USB UVCカメラ、安定した電源、ESP-IDF v5.4.2、ESP32-P4ツールチェーンを用意します。
書き込み前に対象ポート、既存ファームウェア、Wi-Fi設定の保管先を確認してください。

```sh
mkdir -p third_party
git clone https://github.com/fooping-tech/tab5_rtsp_logger.git third_party/tab5_rtsp_logger
git -C third_party/tab5_rtsp_logger checkout e5e5fee79c2232bd5de4a994d8f90e12f4630952
export TAB5_REFERENCE_PATH="$(pwd)/third_party/tab5_rtsp_logger"
```

Wi-Fi認証情報は `firmware/sdkconfig.local` に書きます。このファイルは `.gitignore` 済みです。

```
CONFIG_TAB5_WIFI_SSID="your-ssid"
CONFIG_TAB5_WIFI_PASSWORD="your-password"
CONFIG_EDGE_ENABLE_RTSP=y
```

```sh
cd firmware
idf.py -DSDKCONFIG_DEFAULTS="sdkconfig.defaults;sdkconfig.local" set-target esp32p4
idf.py build
idf.py -p /dev/cu.usbmodemXXXX flash monitor
```

## 設定

`idf.py menuconfig` の `Tab5 Camera Clock` にあります。

| 項目 | 既定 | 説明 |
|---|---|---|
| `EDGE_ENABLE_RTSP` | n | 平文RTSPを有効化する |
| `EDGE_TIMEZONE` | `JST-9` | POSIX TZ文字列。UIより先に適用される |
| `EDGE_SNTP_SERVER` | `pool.ntp.org` | 時刻同期先 |
| `EDGE_SNTP_RESYNC_MINUTES` | 60 | 再同期間隔。1回だけではRTCが漂う |
| `TAB5_UVC_URB_COUNT` / `_KIB` | 12 / 16 | USBアイソクロナス転送の在庫。既定の3個では取りこぼす |
| `TAB5_PREVIEW_MAX_AGE_MS` | 1500 | この時間を超えた映像は表示せず画面を消す |
| `TAB5_JPEG_DECODE_TIMEOUT_MS` | 500 | 1フレームの復号を諦めるまでの時間 |
| `TAB5_RTSP_FPS` | 10 | 復号エンジンを画面と分け合うため15より低い |

## 既知の問題

ESP32-P4のpre-v3シリコン（本体ログの `chip revision`）には、USB DWC OTGのアイソクロナス転送が
他のバスマスタと同時に動くとDMAが誤ったアドレスへ書き込む不具合があります
（[espressif/esp-idf#18235](https://github.com/espressif/esp-idf/issues/18235)）。
本ファームウェアでは `assert failed: spinlock_release`（`components/usb/hcd_dwc.c` のUSBホストISR）による
再起動として現れます。実測で平均8.5分に1回程度です。アプリケーション側では解消できません。

同じ領域で、ハードウェアJPEG復号が稀にタイムアウトします。1フレームが失われるだけで復帰します。

## ライセンス

本リポジトリのコードは **MIT**（[全文](LICENSE)）。
`firmware/main/hal`、`firmware/main/rtsp`、`camera_clock_font_64.c`、`camera_clock_time_88.c` は
[tab5_rtsp_logger](https://github.com/fooping-tech/tab5_rtsp_logger) revision `e5e5fee` からの移植で、
上流のMIT通知は `licenses/tab5_rtsp_logger-MIT.txt` に保持しています。
依存componentのライセンスはそれぞれ保持します。
