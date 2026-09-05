# USB UVC と H.264 RTSP の統合

## 来歴

参照: [tab5_rtsp_logger e5e5fee](https://github.com/fooping-tech/tab5_rtsp_logger/tree/e5e5fee79c2232bd5de4a994d8f90e12f4630952)。
参照checkoutは読み取り専用で、このプロジェクトにアプリケーション/UI全体は組み込みません。

- `firmware/main/hal/hal_uvc.*`: 参照 `platforms/tab5/main/hal/components/hal_uvc.cpp` と `hal/hal_uvc.h` を基に移植。UVCネゴシエーション/profile fallback、3スロットcopy、再接続/stall回復、stop所有権を保持する。LVGL preview/JPEG render bufferはアプリ側へ分離する。残るreclamation workerはready markerだけを解放し、画素変換しない。callback timestampとstream generationをcopy契約に追加し、2消費者はcamera driver bufferを借用しない。
- `firmware/main/rtsp/*` と `tests/rtsp_framing_test.cpp`: RTSP transport、Annex-B、FU-A、RTCP分離、session cleanup、deadlineを移植。MIPI encoding/acquisitionと暗黙のMIPI fallbackを削除する。RTSPはアプリ所有UVC streamを検査し、開始/停止しない。20KiB PSRAM RTSP task stackを保持する。Content-Base/RTP-Infoは参照の固定 `tab5.local` でなく、acceptしたsocketのローカルIPを使う。
- `network.cpp`: ESP-Hosted STA netif start/connected/stop hookを移植し、LAN専用mDNS/RTSP service起動へ適用する。demo web serverは移植しない。切断時はnetif-downと上限付き再接続を行う。
- `board.cpp`: BSP I2C pin 31/32とPI4IOE `0x44`、WLAN/USB 5V register sequence、ESP-IDF USB host event taskを保持する。内蔵カメラ、スピーカー、充電UIは対象外。
- 外部参照component: esp_video 0.7.0、esp_cam_sensor 0.7.1、esp_sccb_intf（すべて参照SHAで固定）。H.264 video deviceだけを有効にし、MIPI/DVP/ISPは無効にする。

参照MIT通知は [licenses/tab5_rtsp_logger-MIT.txt](../licenses/tab5_rtsp_logger-MIT.txt) に保持します。元のSPDX通知も移植元に残します。
アプリケーション全体はAGPL-3.0-onlyです。これは上流componentの再ライセンスや、リンクされたコードとの法的分離を意味しません。

## データフローと制限

USB要求は640x480 @15fps、MJPEG優先・YUY2代替、アプリケーション生存期間中のcaptureです。callbackは最大2MiBを
PSRAMの3スロットの一つへコピーし、busy slotなら無制限キューを作らずdropします。AIとRTSPはそれぞれ独自コピーを持ちます。
AIは報告/JPEG寸法を検査し、入力年齢を1秒以下に制限します。status loopは10秒で証拠を失効し、切断またはstream generation
変更を観測したら直ちにresetします。姿勢は常に `unknown` です。

RTSPは同時1クライアント、H.264 payload type 96、port 8554のTCP interleaved RTP/RTCP、path `/baby` をサポートします。
UDP RTP、認証、TLS、複数クライアントfanoutはありません。設定されたcapture寸法に収まらないfallback formatは使えません。
正確な設定解像度を広告するカメラを使います。名目FPSは測定済みthroughputではありません。YOLO26推論、JPEG復号、RTSP変換は
P4 core、メモリ帯域、PSRAMを競合します。メモリ/stack枯渇や上流コード内のwatchdog abortは、実機試験を完了するまで起き得ます。

## 初回の安全な設定

1. `idf.py menuconfig` でUSB入力（既定）を有効にする。ローカル推論だけならRTSPはoffのままにする。
2. RTSPには私有LANのSSID/パスワードを設定し、明示的なRTSP switchを有効にする。画像アップロードや外部サービスは使わない。
3. build後、対象Tab5のport、board、電源を確認してから手動でflashを承認する。USB-Aへ対応カメラを接続し、Tab5とカメラに十分な安定電源を使う。
4. serialにUVC negotiated/streaming、AI sequence/score/latency、presence/statusが出ることを確認する。DHCP IPを確認し、`rtsp://tab5.local:8554/baby`（またはログのIP）を使う。
5. 転送方式はTCPを選び、port 8554を公開しない。認証情報とbuild artifactは私有扱いにする。
6. このbuildはNVS partitionを含むため既存flash layoutを確認し、NVSを自動消去しない。

## 受入チェックリスト

- UVC enumerationと連続した新しいsequence。MJPEG/YUY2別に回転/色を確認する。
- 既知の立会い場面、人物なしnegative、長い推論中の抜線/reset/replugでAI state transitionを確認する。
- LAN上のRTSP OPTIONS/DESCRIBE/SETUP/PLAY、SPS/PPS/IDR、H.264 decodeを確認し、client disconnectでAIが停止しないことを確認する。
- PLAY中の再接続/遅いclient/RTCP/camera抜線で、以前のgenerationの古いAI結果を出さないことを確認する。
- 推論+配信の遅延/FPS、internal/PSRAM free block最小値、task stack HWM、dropを測る。
- AI+RTSPの立会い1時間試験、その後の延長試験を行う。Scrypted/HomeKitは別検証にする。

自動buildとframing testだけでは実機gateを完了しません。観測済みの事実は [PLANS.md](../PLANS.md) を参照してください。
