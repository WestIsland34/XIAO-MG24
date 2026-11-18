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
 フォルダの GIF ─ 96x64 に縮小・256色に減色・差分圧縮 ─BLE→ RAM (160KB) に保存 → OLED でループ再生
```

### XIAO 側
1. `BLE_GIF_Player/BLE_GIF_Player.ino` を開く
2. **ツール → Protocol stack を「BLE (Silabs)」** にする（None のままだとコンパイルエラーになる）
3. 書き込むと OLED に「Waiting for PC / XIAO-GIF」と表示される

### PC 側（Windows の Chrome または Edge）
- **GitHub Pages**: https://westisland34.github.io/XIAO-MG24/ を開く（Pages を有効にした場合）
- **ローカル**: `docs` フォルダで `python -m http.server 8000` を実行し、http://localhost:8000 を開く
  （Web Bluetooth は https か localhost でしか使えない）

使い方:
1. 「XIAO に接続」→ 一覧から **XIAO-GIF** を選ぶ
2. 「フォルダを選ぶ」で GIF の入ったフォルダを指定（次回からは「前回のフォルダ」ボタンで開ける）
3. GIF をクリックするとプレビュー、「XIAO で再生」（またはダブルクリック）で転送して再生
4. 「順番に切り替え」をオンにすると、指定秒ごとにフォルダ内の GIF を順に再生

メモ:
- 全フレームを 1 つのパレット（最大 256 色）にまとめ、前フレームとの差分だけを送る。
- 変換後に 160KB を超える GIF は、収まるまで 1 枚おきにフレームを間引く（表示時間は合算されるので再生速度は変わらない）。
- XIAO の電源を切るとアニメは消える（RAM に保存しているため）。
- 通信仕様は `BLE_GIF_Player.ino` 冒頭のコメントを参照。

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
