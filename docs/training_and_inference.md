# 学習からTab5推論まで

この手順は、将来の **baby 1-class detector** を作るための運用ガイドです。
現在Tab5で動作するのは、ESP-DL付属のCOCO YOLO26nによる `person` 検出です。学習済みの
赤ちゃんモデルではありません。大人も検出対象になり、`absent` は空室や安全を保証しません。

子どもの画像・映像・学習済み重み・校正画像・実機ログは、すべて私有ストレージに置き、この
リポジトリ、PR、ビルド成果物へ追加しません。

## 全体の流れ

1. 同意済みデータを収集し、セッション単位で分割・アノテーションする。
2. PC上でYOLO26nを1クラス（`baby`）として学習する。
3. テストセットで検出・在室判定を評価し、採用条件を満たすか判断する。
4. ESP-DLの固定revisionのノートブックで量子化・`.espdl`変換する。
5. PCエミュレータとTab5で同一画像の結果を比較する。
6. モデル契約に合わせてファームウェアのアダプタを変更し、個別にビルド、フラッシュ、実機評価する。

各段階で不明、失敗、古いフレームは `unknown` として扱います。変換成功・ビルド成功だけで
精度や安全性は証明されません。

## 1. 私有データセットを準備する

保護者の同意、利用目的、保管期限、削除方法を記録します。通常の見守り環境だけを収集し、
危険な姿勢や状況を意図的に作りません。空のベッド、養育者のみ、ぬいぐるみ、毛布、暗所、
ブレ、隠れを必ず含めます。

同じ子ども・家庭・連続撮影セッションが train/val/test にまたがらないよう、**フレーム抽出前**に
分割します。開始時の目安は 70/15/15 ですが、データ量よりこの分離を優先します。

```text
/private/tab5-baby-v1/
  images/{train,val,test}/<session>_<frame>.jpg
  labels/{train,val,test}/<session>_<frame>.txt
  splits.sha256
  manifest-private.csv
```

ラベルはYOLO形式です。`baby` はクラス `0`、赤ちゃんがいない画像には空の同名ラベルファイルを
置きます。各ボックスを人手で確認し、曖昧・隠れ・対象外の理由は私有manifestへ記録します。

私有領域に次のYAMLを作成します（この絶対パスをGitへ保存しません）。

```yaml
path: /private/tab5-baby-v1
train: images/train
val: images/val
test: images/test
names:
  0: baby
```

## 2. PCで学習する

Python 3.10または3.11の隔離環境で実行します。GPU/RAMに応じて `batch` を下げます。
`epochs=100` と `batch=8` は開始値であり、固定された最適値ではありません。

```sh
git clone https://github.com/fooping-tech/tab5_baby_edge_ai_agpl.git
cd tab5_baby_edge_ai_agpl
python3 -m venv .venv
. .venv/bin/activate
pip install -r requirements-training.txt

yolo detect train \
  model=yolo26n.pt \
  data=/private/tab5-baby-v1/baby.yaml \
  imgsz=512 epochs=100 batch=8 seed=42 \
  project=/private/tab5-artifacts/train name=baby_v1
```

初回の `yolo` 実行は上流の事前学習重みを取得することがあります。使用前に取得元、SHA256、
Ultralytics/ESP-DL revision、データsplitハッシュ、コマンド、seed、依存バージョンを私有の
モデルmanifestへ記録します。

学習の過学習判定はvalで行います。最終テストセットは、閾値・重み・前処理を固定するまで使いません。

## 3. 評価する

```sh
yolo detect val \
  model=/private/tab5-artifacts/train/baby_v1/weights/best.pt \
  data=/private/tab5-baby-v1/baby.yaml split=test imgsz=512
```

少なくともmAP50、mAP50-95、precision、recall、赤ちゃん不在時の誤検出を記録します。さらに、
同一のテストセッションについて在室状態を独立ラベルし、本リポジトリの集計器で確認します。

```sh
python3 tools/evaluate_presence.py /private/tab5-evaluation.csv
```

CSV形式と集計値の意味は [evaluation.md](evaluation.md) を参照してください。暗所、逆光、毛布、
部分隠れ、養育者、ぬいぐるみ、距離、カメラ角度ごとに分けて評価します。`unknown` は成功扱いにせず、
recallのmissとして数えます。

## 4. ESP-DLへ量子化・変換する

現在のファームウェアは、512x512 RGB入力、`one2one_p3/p4/p5` の6ヘッド、各クラスヘッド80チャネルの
COCOモデルだけを受け入れます。**`best.pt` や一般的なONNXをコピーしてもTab5では動きません。**

ESP-DLの固定revisionを用意し、同梱チュートリアルを私有の作業ディレクトリで実行します。

```sh
git clone https://github.com/espressif/esp-dl.git /private/esp-dl
git -C /private/esp-dl checkout 5d9c36063dddbe98b5387828c831d6bbadb1370f
export ESP_DL_PATH=/private/esp-dl
cd "$ESP_DL_PATH/examples/tutorial/how_to_quantize_model/quantize_yolo26"

python3 -m venv /private/tab5-quant-env
. /private/tab5-quant-env/bin/activate
pip install -r requirements.txt
pip install jupyterlab
jupyter lab quantize_yolo26_coco.ipynb
```

ノートブックは同じディレクトリへ私有コピーして編集します。ダウンロード/アップロード用セルを無効化し、
少なくとも以下を確認します。

- `IMG_SZ_I=512`、`PLATFORM="p4"`、データYAMLと `PT_FILE` は私有のbabyデータ/重みを参照する。
- 校正はtrainだけを使い、val/testを混ぜない。代表画像の並びとハッシュを保存する。
- 非正方形カメラ画像では、校正前処理をTab5と同じRGB letterbox（padding 114）にする。
- 出力は1クラス、batch 1、NHWC、`one2one_*_box` と `one2one_*_cls` の6ヘッドである。
- エミュレータ結果、量子化設定、入出力名/shape/dtype/scale、`.espdl`のSHA256を私有manifestへ保存する。

詳細な注意点は [model_pipeline.md](model_pipeline.md) を参照してください。

## 5. Tab5へ反映して推論する

変換済み`.espdl`を公開リポジトリへ入れません。まず私有の同一画像でFP32、ESP-DLエミュレータ、
Tab5のボックス・スコア・座標を比較します。

現在の `firmware/main/detector.cpp` はCOCOの80クラスを明示的に検証し、クラス0を`person`として使います。
baby 1-classモデルを有効にするには、次を一つのレビュー対象として同時に変更します。

1. `firmware/main/CMakeLists.txt` のモデル選択とSHA256検証。
2. `firmware/main/detector.cpp` のクラス数 `80`、ラベル名、対象クラス、ヘッドshape検証。
3. モデル入力サイズ、letterbox/正規化、座標逆変換、閾値。
4. モデルmanifestと保持評価結果。
5. unknownへのフェイルセーフ、USB切断/古いフレーム/メモリ不足時の挙動。

変更後はホストテスト、ESP-IDFビルド、手動承認したフラッシュ、静止画比較、USBライブ入力、RTSP同時動作、
再接続、長時間試験を別々に記録します。`supine/not_supine/unknown` は別分類器と別評価が必要であり、
baby検出モデルの出力から推測しません。

## 実機での最小確認

1. モデルSHA256、ファームウェアSHA256、IDF revision、設定値を記録する。
2. 権利を確認した静止画で、期待したbox/score/座標を確認する。
3. 同一画像のPCエミュレータ結果とTab5の結果を比較する。
4. USBカメラではフレーム時刻、推論時間、unknown率、誤検出、切断/再接続を記録する。
5. RTSP、Scrypted、画面、実機の長時間動作はモデル精度とは別に受入する。

このPoCは見守り・医療・安全アラームの判断機能ではありません。精度評価後も、人による確認と
安全な運用を前提にしてください。
