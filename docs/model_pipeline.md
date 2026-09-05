# 私有データから学習・量子化・ESP-DL変換まで

初回の実行手順は [training_and_inference.md](training_and_inference.md) を参照してください。
この文書はデータ管理とESP-DL変換時の詳細な制約を記録します。

## 1. 収集とアノテーション

- 映る全員について保護者の同意と権利を確認し、アクセス制限、暗号化保管、削除期限を決める。
- 同意済みカメラでローカル収集し、クラウドアップロード、Roboflow APIキー、自動収集をこのリポジトリで行わない。
- 例として5秒ごとに1フレーム程度を私有 `data/` へ保存し、セッションID、仮名、時刻、カメラ/ROI/回転、照明、距離、隠れ、同意記録の参照を私有manifestへ残す。
- 通常の立会い環境だけを収集し、危険な乳児姿勢を意図的に作らない。
- 空のベッド、養育者のみ、人形、毛布、暗所、ブレ、部分表示をnegative/challengeとして含める。
- YOLOラベルは正規化した `class_id center_x center_y width height`。babyはクラス0で、空フレームは空のラベルファイルにする。box境界、画像対応、曖昧例を人手確認する。
- 子ども/家庭/セッションでの分割は、フレーム抽出やaugmentationより**先**に行う。開始時の目安は70/15/15で、グループを交差させず、画像とsplit一覧のSHA256を残す。
- 校正にはtrainの代表サブセットだけを使う。valは調整、testは最終評価まで使わない。
- 姿勢ラベルは検出boxと別に保持し、`supine/not_supine/unknown`、不確実さ、視認性の理由を記録する。
- 私有画像、映像、生ログ、学習重み、画像を埋め込んだファームウェアはコミットしない。

私有データセットの例:

```text
images/{train,val,test}/<session>_<frame>.jpg
labels/{train,val,test}/<session>_<frame>.txt
```

`configs/baby.example.yaml` を私有領域へコピーし、絶対データセットrootを編集します。同意metadataは分離します。

## 2. 将来のbaby検出器を学習する

Python 3.10/3.11の隔離環境を使います。利用可能なRAM/GPUに応じてbatchを調整し、インストール前に依存を確認します。

```sh
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements-training.txt
yolo detect train model=yolo26n.pt data=/absolute/private/baby.yaml imgsz=512 epochs=100 batch=8 seed=42 project=artifacts/train name=baby_v1
yolo detect val model=artifacts/train/baby_v1/weights/best.pt data=/absolute/private/baby.yaml split=test imgsz=512
pip freeze > artifacts/training-environment.txt
```

最初のコマンドは上流の事前学習重みを取得する可能性があります。取得URLとSHA256を記録します。
epochs/batchは開始値であり、検証済みのハイパーパラメータではありません。設定、seed、split hash、学習コードrevision、
モデルhash、val/test指標、失敗例を保存します。babyのみの場面と養育者のみの場面を分けて評価します。

## 3. 量子化と変換（上流ノートブックによる手動作業）

標準person PoCには学習/変換は不要で、固定されたzooの`.espdl`を使います。再学習重みには固定したESP-DLチュートリアル
`$ESP_DL_PATH/examples/tutorial/how_to_quantize_model/quantize_yolo26/` を使います。一般的な
`yolo export format=onnx` の結果をそのまま互換とみなしてはいけません。YOLO26 decoderは一般的な後処理済みtensorではなく、
名前付きのbox/class 6ヘッドを要求します。

```sh
cd "$ESP_DL_PATH/examples/tutorial/how_to_quantize_model/quantize_yolo26"
python3 -m venv /absolute/private/quant-env
. /absolute/private/quant-env/bin/activate
pip install -r requirements.txt
pip install jupyterlab
jupyter lab quantize_yolo26_coco.ipynb
```

ノートブックは同一ディレクトリへ私有コピーして編集します。相対 `scripts/` / `esp_ppq_lut/` importのためです。
セルを確認して、データセットのダウンロード/fallbackとRoboflowのupload/downloadを無効にします。COCOノートブックでは
`IMG_SZ_I=512`、`PLATFORM="p4"`、`DATA_YAML_FILE_I` を私有baby YAMLへ設定します。`QATConfig` の `PT_FILE` を学習済み
`best.pt`、`DATA_FALLBACK_PATH` を私有train画像、`ESPDL_OUTPUT_DIR` を私有明示パスへ設定し、校正batch/stepsはメモリに合わせます。

loaderがtrain splitを使い、1クラスになり、COCOラベル/評価設定を残していないことを確認します。固定した
`scripts/dataset.py` は正方形Resizeを使い、デバイスのletterboxとは異なります。非正方形フレームはRGB letterbox、
padding 114、同じ補間へ私有loaderを変更するか、512x512 RGBにletterbox済みのtrain専用ディレクトリを校正に使います。
変更を保存し、採用前にデバイス/エミュレータ前処理とtensorを比較します。

loaderは選択ディレクトリのみ（ネストしたセッションは非対象）を走査し、非ソート順を打ち切ります。平坦で決定的な代表サブセットを
渡し、順序付きファイル一覧/hashを保存します。最初は `INT16_LUT_STEP_I=32` を使い、編集済みノートブック、設定、Python lockを保存します。

上流工程は、patched one2one ONNX export、calibration、TQT scale refinement、passive/alignment pass、
mixed INT8/INT16 LUT fusion、6ヘッドgraph surgery、`.espdl` exportの順に行います。出力名
`one2one_p3_box` / `one2one_p3_cls` からp5、box 4チャネル、baby class 1チャネル、batch 1 NHWC、box/class対の
INT8/INT16 dtypeを確認します。標準COCOのclassチャネルは80です。入力shape、出力名、scale、正規化、targetを
モデルmanifest templateへ記録します。

ノートブックエミュレータで同一の私有hold-out画像を量子化前後比較し、同じ画像でデバイスのbox/scoreも比較します。
letterbox padding、RGB順序、逆座標変換を確認します。調整前に許容recall/mAP低下を決め、変換成功を精度成功と主張しません。
このリポジトリはノートブックを自動化/検証せず、babyモデルも配布しません。導入には [architecture.md](architecture.md) の
明示的なアダプタ変更が必要で、COCOモデルのファイル名を変えるだけでは不十分です。

上流参照: [固定revisionのYOLO26変換チュートリアル](https://github.com/espressif/esp-dl/tree/5d9c36063dddbe98b5387828c831d6bbadb1370f/examples/tutorial/how_to_quantize_model/quantize_yolo26)。
