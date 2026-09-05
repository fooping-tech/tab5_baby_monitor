# Tab5 Baby Edge AI (AGPL research PoC)

ESP32-P4 / M5Stack Tab5 向けの人物検出・在室推定の初期実装です。
**現段階は静止画のYOLO26n推論と在室フィルター。ライブカメラ接続は未実装です。**
person は大人も含み、赤ちゃんの識別ではありません。`absent` は検出証拠がないという推定であり、空室の保証ではありません。
呼吸・窒息・SIDS・睡眠の安全性を判定するものではなく、見守りや医療機器の代替に使用しないでください。

## 実装範囲

| 機能 | 状態 |
|---|---|
| 公式 COCO80 YOLO26n / 512 / P4 モデルのRGB888推論 | 実装済み、検証状況は PLANS.md |
| personクラス0の最大スコア → present/absent/unknown | ホストでテスト可能 |
| 起動・エラー・古いフレーム → unknown | 実装済み |
| 同意済みJPEGを1枚埋め込むスモークテスト | 実装済み。時間的な在室判定はunknownのままが正常 |
| Tab5内蔵/USB-UVCカメラ、FrameBroker、画面、SD、RTSP | 未接続。他リポジトリのコードはコピーしていない |
| baby 1-class detector | 学習・変換手順のみ。重みなし |
| supine / not_supine / unknown | 拡張契約のみ。出力は常にunknown |

## ホストテスト（モデル・画像不要）

```sh
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
python3 -m unittest discover -s tests -p 'test_*.py'
python3 tools/evaluate_presence.py tests/fixtures/presence.csv
```

## ESP32-P4ビルド

ESP-IDF **v5.4.2** とP4ツールチェーンをインストールし、その環境を有効にしてください。
Tab5の16MB flash / PSRAMを前提とします。EV board BSPは使わず、カメラ・LCDの初期化もしません。
USB Serial/JTAGをログ出力先とし、watchdogは無効化していません。実際のハードウェアでのタイミング調整は別途必要です。

```sh
mkdir -p third_party
git clone https://github.com/espressif/esp-dl.git third_party/esp-dl
git -C third_party/esp-dl checkout 5d9c36063dddbe98b5387828c831d6bbadb1370f
export ESP_DL_PATH="$(pwd)/third_party/esp-dl"
cd firmware
idf.py -DPOC_JPEG=/absolute/path/to/consented-test.jpg set-target esp32p4
idf.py -DPOC_JPEG=/absolute/path/to/consented-test.jpg build
```

画像はファームウェアに埋め込まれます。子どもの画像を使った `.bin` やbuild成果物を公開しないでください。
公開検証には権利を確認した非機微なサンプルを使ってください。
ビルドはネットワークからESP-IDF管理依存を取得します。mainのmanifestでバージョンを固定し、
生成される `firmware/dependencies.lock` は機種固有のパスを含むためGit管理せず、検証記録とともにローカル保存します。
ESP-DLとYOLO26は上記のGit revisionで固定し、異なるrevisionはCMakeが拒否します。
変更した依存checkoutの内容は利用者がレビューしてください（HEAD一致だけでは作業ツリーの改変は検知しません）。

書き込みは対象ポートと既存ファームウェアの退避を確認した後に手動で実行します：

```sh
idf.py -p /dev/your-confirmed-port flash monitor
```

起動後に `valid`、`person_score`、`pipeline_us`、`presence`、`posture`、`free_heap` を1回ログ出力します。
静止画を再利用して連続フレーム扱いにすることはありません。在室フィルターの2秒/5秒の継続条件はライブ入力接続後に評価します。

## 構成と次段階

- `core/presence.hpp`: 独立した時間フィルター（入力閾値0.5、入室2秒、退室5秒、鮮度10秒）。値は未調整のPoC既定値。
- `firmware/main/detector.*`: 公式ESP-DL前後処理、COCO80モデル契約、借用RGB888フレーム境界。
- [データ収集・学習・量子化](docs/model_pipeline.md)
- [カメラ・baby・姿勢の拡張契約](docs/architecture.md)
- [評価と実機受入](docs/evaluation.md)
- [依存とライセンス](docs/dependencies.md)、[実施記録](PLANS.md)

## ライセンス

本リポジトリのコードは **AGPL-3.0-only**（[全文](LICENSE)）。依存のライセンスはそれぞれ保持します。
Ultralytics由来のコード・モデルを使うため、配布やネットワーク提供の際は対応ソースと通知の要件を確認してください。
別リポジトリ/APIに分けるだけで、組み合わせたファームウェアのAGPL義務が消えるとは扱いません。
詳細は [Ultralytics公式](https://www.ultralytics.com/license) と [GNU AGPL](https://www.gnu.org/licenses/agpl-3.0.html) を確認してください。
