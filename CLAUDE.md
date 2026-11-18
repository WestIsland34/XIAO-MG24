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
- コンパイル確認: `arduino-cli compile -b SiliconLabs:silabs:xiao_mg24:protocol_stack=ble_silabs BLE_GIF_Player`（RAM 約74%使用、受信バッファ160KB）。
- 実機で転送・再生を確認済み（2026-11-19）。転送は「4パケットに1回、応答ありで書く」流量制御。応答なし書き込みを流し続けると XIAO の BLE バッファ(約3KB)が詰まり、ACK 通知が出せなくなる。
- 描画は 8x8 タイル単位の差分。シリアルに `draw: ... ns/px, late>10ms ...` を出す（理論値 2000ns/px @8MHz）。

## 作業の注意
- 新しい表示器・センサーを扱うときは、ピン表記から推測せず型番とコントローラICを先に確認する。
- Arduino スケッチは「フォルダ名 = .ino 名」で置く。
- 別プロジェクト（例: ../HangOff は XIAO nRF52840 mbed 用）と環境を混同しない。
