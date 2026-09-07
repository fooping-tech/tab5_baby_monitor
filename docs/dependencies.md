# 依存関係

| 対象 | 固定 | ライセンス |
|---|---|---|
| ESP-IDF | v5.4.2 | Apache-2.0 |
| tab5_rtsp_logger | `e5e5fee79c2232bd5de4a994d8f90e12f4630952` | MIT |
| LVGL | 9.2.2 | MIT |
| espressif/usb_host_uvc | 2.5.1 | Apache-2.0 |
| espressif/esp_h264 | 1.0.4 | Espressif |
| espressif/esp_new_jpeg | 1.0.2 | Espressif |
| espressif/esp_hosted / esp_wifi_remote | 1.4.0 / 0.8.5 | Apache-2.0 |
| espressif/mdns | 1.8.2 | Apache-2.0 |
| esp_lcd_* / esp_codec_dev | idf_component.yml参照 | Apache-2.0 |

`third_party/` はGit管理外です。`TAB5_REFERENCE_PATH` に固定revisionのチェックアウトを指定します。
revisionが一致しないとビルドは停止します。

本リポジトリのコードはMITです。上流componentの再ライセンスや、リンクされたコードとの
法的分離を意味しません。第三者通知は `licenses/` に保持します。

## 以前のAI依存について

このファームウェアはかつてESP-DLとUltralytics由来のYOLO26n重みを含んでおり、
Ultralyticsのライセンスに従いリポジトリ全体をAGPL-3.0-onlyとしていました。
推論機能を削除し、Ultralytics由来の成果物への依存が無くなったため、MITへ変更しています。
AGPL版として配布済みのリビジョンは、その受領者に対してはAGPLのままです。
