# CLAUDE.md

XIAO MG24 (EFR32MG24) で遊ぶ Arduino プロジェクト。詳細は README.md。

## 確定事項（動作確認済み 2026-11-18）
- OLED は **秋月 QT095B = SSD1331 96x64 RGB カラー, SPI**。SSD1306/SH1106 ではない。U8g2 は使わず **Adafruit_SSD1331 + Adafruit_GFX** を使う。
- 配線: GND-GND, VCC-3V3, SCL-D8(SCK), SDA-D10(MOSI), RES-D3, DC-D2, CS-D1
- ボード: Silicon Labs コア / Seeed Studio XIAO MG24 (Sense) / Protocol stack: None
- ハードウェアSPI `Adafruit_SSD1331(&SPI, cs, dc, rst)` 8MHz で動作確認済み（2026-11-19）。D8/D10 が XIAO の SPI ピン。
- ソフトウェアSPIも映るが、この CPU では Adafruit に高速 I/O が無く digitalWrite で1ビットずつ送るので非常に遅い。動画用途では使わない。
- 色は RGB565 (16bit)。

## BLE GIF Player
- XIAO 側 `BLE_GIF_Player/` は Protocol stack **BLE (Silabs)**（sl_bt_* API, 動的 GATT）。PC 側は `docs/index.html` 1ファイル（Web Bluetooth + ImageDecoder, ビルド不要）。
- 通信仕様・アニメ形式は .ino 冒頭コメントが正。変えるときは .ino と index.html の両方を直す。
- コンパイル確認: `arduino-cli compile -b SiliconLabs:silabs:xiao_mg24:protocol_stack=ble_silabs BLE_GIF_Player`（RAM 約23%）。
- フラッシュは複数アニメのキャッシュ (リングバッファ, 目録 63 件を 0x0/0x1000 に交互書き, 再生中 id は 0x2000 に追記ログ, データは 0x3000〜)。BEGIN に CRC と名前を付け、同じ CRC があれば転送せずに再生 (DONE cached=1)。LIST (0x05) / PLAY (0x06) で一覧・再生。
- 加速度センサー: XIAO MG24 Sense の LSM6DS3TR-C は **Wire1 (I2C0, PB2/PB3), アドレス 0x6A, 電源 PD5 (ピン19) を HIGH**。WHO_AM_I=0x6A を確認済み。1 分動きがないと `enableDisplay(false)` でスリープ、0.05g 以上の変化か BLE 操作で復帰。
- Web の「ディフォルト」は GitHub API で `GIF/` を列挙 (IP ごと 60 回/時。会社の回線では使い切っていた) → 失敗したら jsDelivr の data API。中身は raw.githubusercontent.com。
- バージョン: .ino の FW_VER_* と index.html の APP_VERSION。INFO の末尾 3 バイトが FW バージョン。
- アニメは基板上の外付けフラッシュに保存: SPI1 (PB0/PB1/PA0) + CS=PA6(ピン21)。JEDEC ID `85 60 16` = Puya 4MB。ボード定義の mx25 shutdown 設定 (PC1-4) は実配線と違うので無視。
- **コア SPI ライブラリ 5.0.0 の不具合**: `transfer(buf, n)`（送受信）は 2048 バイト超を分割するとき位置をずらさない → 2048 バイト以下に分けて呼ぶ。送信のみの `transfer(buf, n, true)` は正しい。
- 実機で転送・保存・再生・電源再投入後の自動再生を確認済み（2026-11-19）。
- 転送は「12パケットに1回、応答ありで書く」流量制御。RX はアプリ管理値 (user_write_request) で、受信はキューに積んで loop() がフラッシュに書く。キューに空きがあれば応答はすぐ返す。応答なし書き込みを流し続けると XIAO の BLE バッファ(約3KB)が詰まる。
- 速度の実測 (Windows + Chrome): 接続間隔 15ms・1M PHY・MTU 247 で約 14KB/s。Chrome の書き込み 1 回 17〜20ms が上限。7.5ms 固定や同時書き込み (Chrome が失敗する) は効果なし。
- このコアの SPI は転送1回ごとに DMA を起動する（1回約25us）。Adafruit の writePixels は1画素ずつ送るので約26us/px と遅い。画素はバッファに詰めて `SPI.transfer(buf, n, true)` で一括送信する（DMA 1回の上限 2048 バイト）。
- 描画は 8x8 タイル単位の差分。8行の帯ごとに変化範囲を1矩形にまとめて送る。シリアルに `draw: ... ns/px, late>10ms ...` を出す（理論値 2000ns/px @8MHz）。

## 作業の注意
- 新しい表示器・センサーを扱うときは、ピン表記から推測せず型番とコントローラICを先に確認する。
- Arduino スケッチは「フォルダ名 = .ino 名」で置く。
- 別プロジェクト（例: ../HangOff は XIAO nRF52840 mbed 用）と環境を混同しない。
