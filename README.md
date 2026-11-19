# XIAO MG24 + カラーOLED (SSD1331)

Seeed Studio XIAO MG24 に 0.95インチ カラー有機EL（秋月電子 114435 / QT095B）をつないで遊ぶプロジェクト。

- OLED: SSD1331, 96×64ドット RGB, SPI専用, 電源 3〜5V
  - 商品ページ: https://akizukidenshi.com/catalog/g/g114435/
  - 注意: ピン表記が `SCL` / `SDA` だが **I2Cではなく SPI**。SSD1306/SH1106 ではないので U8g2 では映らない。

## フォルダ構成

| パス | 内容 |
|---|---|
| `OLED_SSD1331_Test/OLED_SSD1331_Test.ino` | 動作確認スケッチ（赤→緑→青→文字表示→動く四角） |
| `BLE_GIF_Player/BLE_GIF_Player.ino` | PC から Bluetooth で受け取った GIF アニメを OLED でループ再生 |
| `docs/index.html` | PC 側アプリ（ブラウザ / Web Bluetooth）。フォルダ内の GIF を選んで XIAO に送る |
| `GIF/` | サンプルの GIF。PC 側アプリの「ディフォルト」で一覧に出る |
| `CLAUDE.md` | Claude Code 用のプロジェクトメモ |

## 配線

| OLED | XIAO MG24 | 役割 |
|---|---|---|
| GND | GND | グランド |
| VCC | 3V3 | 電源 |
| SCL | D8 (SCK) | SPIクロック |
| SDA | D10 (MOSI) | SPIデータ |
| RES | D3 | リセット |
| DC | D2 | データ/コマンド切替 |
| CS | D1 | チップセレクト |

D9 (MISO) は未使用。

## 新しいPCでのセットアップ

1. **Arduino IDE 2.x** をインストール（https://www.arduino.cc/en/software）
2. **ファイル → 基本設定 → 追加のボードマネージャのURL** に追加:
   ```
   https://siliconlabs.github.io/arduino/package_arduinosilabs_index.json
   ```
3. **ボードマネージャ** で「Silicon Labs」をインストール
4. **ライブラリマネージャ** で以下をインストール（依存ライブラリは「全てインストール」）
   - Adafruit SSD1331 OLED Driver Library for Arduino
   - Adafruit GFX Library
5. **ツール** の設定
   - ボード: Silicon Labs → **Seeed Studio XIAO MG24 (Sense)**
   - Protocol stack: **None**（Matter/BLE を入れるとフラッシュが約900KBに膨らむ）
   - ポート: XIAO の COM ポート
6. `OLED_SSD1331_Test/OLED_SSD1331_Test.ino` を開いて書き込み

書き込みログの最後に `** Programming Finished **` と出れば成功（OpenOCD の `halted due to debug-request` はエラーではない）。

## BLE GIF Player（PC のフォルダの GIF を Bluetooth で再生）

```
PC (Chrome / Edge)                                  XIAO MG24
 フォルダの GIF ─ 96x64 に縮小・256色に減色・差分圧縮 ─BLE→ 基板上の外付けフラッシュ (4MB) に保存 → OLED でループ再生
```

### XIAO 側
1. `BLE_GIF_Player/BLE_GIF_Player.ino` を開く
2. **ツール → Protocol stack を「BLE (Silabs)」** にする（None のままだとコンパイルエラーになる）
3. 書き込むと OLED に「Waiting for PC / XIAO-GIF」と表示される

### PC 側（Windows の Chrome または Edge）
- **GitHub Pages**: https://westisland.jp/XIAO-MG24/ を開く（必ず https:// で）
- **ローカル**: `docs` フォルダで `python -m http.server 8000` を実行し、http://localhost:8000 を開く
  （Web Bluetooth は https か localhost でしか使えない）

使い方:
1. 「XIAO に接続」→ 一覧から **XIAO-GIF** を選ぶ
2. GIF の一覧を出す（3 種類）
   - **ディフォルト**: このリポジトリの `GIF/` フォルダの GIF（GitHub から読む）
   - **フォルダを選ぶ**: PC のフォルダを指定（次回からは「前回のフォルダ」ボタンで開ける）
   - **キャッシュを選ぶ**: XIAO のフラッシュに残っている GIF（接続が必要）。送らずにすぐ再生できる
3. GIF をクリックするとプレビュー、「XIAO で再生」（またはダブルクリック）で転送して再生
4. 「順番に切り替え」をオンにすると、指定秒ごとに一覧の GIF を順に再生
5. タイトルの横に Web アプリのバージョン、接続後は XIAO のファームウェアのバージョンも出る

メモ:
- 全フレームを 1 つのパレット（最大 256 色）にまとめ、前フレームとの差分だけを送る。
- XIAO 基板上の外付けフラッシュ（4MB, SPI1 / CS=PA6）に保存する。1 個あたり約 4MB まで間引かずに送れる。
- 送った GIF はフラッシュにキャッシュとして残る（最大 63 個、約 4MB。一杯になると古いものから上書き）。
  同じ GIF をもう一度送ると、転送せずにキャッシュから再生する。
- 電源を入れ直すと、最後に再生していたアニメを確認（CRC）してから自動で再生する。
- 加速度センサー（XIAO MG24 **Sense** の LSM6DS3）が **1 分間動きを感じないと画面を消してスリープ**し、
  動かすか PC から操作すると再開する。Sense でない XIAO MG24 ではスリープしない。
- 転送速度は Windows + Chrome で約 14KB/s（200KB の GIF で約 15 秒）。Chrome の 1 回の書き込みに約 17ms かかるのが上限で、接続間隔を短くしても速くならなかった。
- 起動時に有機 EL に `Flash 4096KB OK` と出る。`NG` ならフラッシュの読み書きに失敗している。
- 通信仕様は `BLE_GIF_Player.ino` 冒頭のコメントを参照。

### 画面を映す（ライブ表示, v1.2.0〜）
PC の画面の一部を 96x64 に縮めて、そのまま有機 EL に映す（フラッシュには保存しない）。
1. 接続後、「画面を選ぶ」で映したい画面・ウィンドウを選ぶ
2. プレビューの枠（3:2）をドラッグで動かし、スライダーかホイールで大きさを変える
3. 「XIAO に映す」で開始、もう一度押すと止めて、最後に再生していたアニメに戻る

- 色は固定 256 色（R6×G7×B6 + 灰色 4）。前のコマとの差分だけを送り、変化がないコマは送らない。
- 抜けがあると XIAO が「キーフレームをください」(0x87) を返し、PC は次に全画面を送る。
- ボトルネックは Bluetooth（約 14KB/s）。画面全体が変わると 2〜3 コマ/秒、時計やカーソルのような小さな変化なら 20 コマ/秒以上の見込み。
  XIAO の描画は全画面でも約 12ms なので足を引っ張らない。シリアルに 2 秒ごと `live: ○ fps, ○ B/frame, draw ○ us/frame, dropped ○` を出す。

## トラブルシューティング

| 症状 | 対処 |
|---|---|
| ポートが出ない | データ通信対応の USB-C ケーブルに替える |
| シリアルに何も出ない | Protocol stack を None に。115200bps を確認。リセットボタン（R）を押す |
| 画面が真っ暗 | 配線（特に D8/D10）を確認。ライブラリが SSD1331 用か確認 |
| ハードウェアSPIで映らない | スケッチ内のソフトウェアSPI版コンストラクタに戻す |
| 接続の一覧に XIAO-GIF が出ない | Protocol stack が BLE (Silabs) か確認。Windows の Bluetooth がオンか確認。一度 Windows の設定でペアリング済みなら削除してみる |
| 「Web Bluetooth が使えません」と出る | Chrome / Edge で https か localhost で開く（file:// では使えない） |
| 会社PCで書き込めない | USB/COMポートやドライバのインストールが社内ポリシーで制限されていないか確認 |
