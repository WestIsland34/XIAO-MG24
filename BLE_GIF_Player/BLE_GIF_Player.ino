/*
 * XIAO MG24 BLE GIF Player
 *
 * PC のブラウザ (docs/index.html, Web Bluetooth) から送られてきた
 * GIF アニメ (96x64 / 256色に変換済み) を受信して XIAO 基板上の外付けフラッシュ
 * (SPI1, CS=PA6) に保存し、SSD1331 OLED でループ再生する。
 * 電源を入れ直しても、保存されているアニメを自動で再生する。
 *
 * ボード         : Silicon Labs → Seeed Studio XIAO MG24 (Sense)
 * Protocol stack : BLE (Silabs)   ← None ではないので注意
 * ライブラリ     : Adafruit SSD1331 OLED Driver Library for Arduino, Adafruit GFX Library
 *
 * 配線 (OLED → XIAO MG24)
 *   GND → GND, VCC → 3V3, SCL → D8 (SCK), SDA → D10 (MOSI),
 *   RES → D3, DC → D2, CS → D1
 *
 * ---- 通信プロトコル (リトルエンディアン) ----
 * Service  7e4a1000-5c3b-4f6e-9a21-3d8b6c0f1a20
 *   RX   ...1001  Write / Write Without Response  (PC → XIAO)
 *          0x01 BEGIN  u32 totalLen
 *          0x02 DATA   u32 offset, payload...
 *          0x03 END    u32 crc32 (IEEE, 全データ)
 *          0x04 STOP
 *          PC は DATA を 4 個に 1 個「応答あり」で書き込み、その応答を待ってから
 *          次を送る。XIAO はフラッシュに書き終わってから応答を返す (= 流量制御)。
 *   TX   ...1002  Notify (XIAO → PC)
 *          0x81 ACK    u32 receivedBytes    (BEGIN の受付時)
 *          0x82 NAK    u32 expectedOffset   (抜けを検出。この位置から送り直して)
 *          0x83 DONE   u8 result (0=OK 1=CRC 2=FORMAT 3=TOO_LARGE 4=NO_FLASH)
 *   INFO ...1003  Read: u8 ver, u16 mtu, u32 maxBytes, u8 width, u8 height
 *
 * ---- アニメデータ形式 ----
 *   "GA01"(4) u8 w, u8 h, u16 frames, u16 nColors, u16 reserved,
 *   palette: nColors x u16 (RGB565),
 *   frame  : u16 delay_ms, u16 len, data[len]  を frames 個
 *   frame data は 96x64 のパレット番号を前フレームとの差分で符号化:
 *     00nnnnnn          : SKIP    n+1 画素 (前フレームのまま)
 *     01nnnnnn v        : RUN     n+1 画素を色 v で塗る
 *     10nnnnnn v...     : LITERAL n+1 画素ぶんの色番号
 *     11nnnnnn nnnnnnnn : 長いSKIP 14bit+1 画素
 *   1枚目は「全画素 0」から の差分。
 *
 * ---- フラッシュの使い方 ----
 *   0x000000 : ヘッダ "GAF1"(4) u32 total, u32 crc32   (受信と検証が済んでから書く)
 *   0x001000 : アニメデータ本体 (受信しながら 4KB ずつ消去して書く)
 */
#include <SPI.h>
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1331.h>

#ifndef ARDUINO_SILABS_STACK_BLE_SILABS
  #error "ツール → Protocol stack で 'BLE (Silabs)' を選んでください"
#endif

// ---------------- OLED ----------------
#define OLED_CS    D1
#define OLED_DC    D2
#define OLED_RST   D3
#define OLED_MOSI  D10   // 基板表記 SDA
#define OLED_SCLK  D8    // 基板表記 SCL

// 1: ハードウェアSPI (D8=SCK, D10=MOSI はXIAOのSPIピンそのもの。速い)
// 0: ソフトウェアSPI (この CPU では digitalWrite で1ビットずつ送るのでとても遅い)
#define USE_HW_SPI 1
#define SPI_HZ     8000000u   // SSD1331 の仕様上限付近。乱れるなら 4000000 に下げる

#if USE_HW_SPI
Adafruit_SSD1331 display(&SPI, OLED_CS, OLED_DC, OLED_RST);
#else
Adafruit_SSD1331 display(OLED_CS, OLED_DC, OLED_MOSI, OLED_SCLK, OLED_RST);
#endif

#define SCR_W 96
#define SCR_H 64
#define NPIX  (SCR_W * SCR_H)

#define PROTO_VER  2
static const char DEVICE_NAME[] = "XIAO-GIF";

static uint8_t  g_idx[NPIX];          // 現在のフレーム (パレット番号)
static uint8_t  g_shown[NPIX];        // いま画面に出ている内容 (差分描画用)
static bool     g_forceFull = true;   // 次のフレームは全画面を描く
static uint16_t g_pal[256];
static uint16_t g_line[SCR_W];

// 差分描画: 画面を 8x8 のタイルに分け、変化したタイルだけ送る
#define TILE   8
#define TILES_X (SCR_W / TILE)
#define TILES_Y (SCR_H / TILE)

// ---------------- 外付けフラッシュ (SPI1) ----------------
#define FLASH_CS        21          // PA6 (pins_arduino.h の SS1)
#define FLASH_HZ        8000000u
#define FLASH_HDR_ADDR  0x000000u
#define FLASH_DATA_ADDR 0x001000u
#define FLASH_SECTOR    4096u
#define FLASH_PAGE      256u
#define FRAME_MAX       8192u       // 1 フレームの差分データの上限 (実際は最大 6.3KB 程度)

static SPISettings g_flashSpi(FLASH_HZ, MSBFIRST, SPI_MODE0);
static uint32_t g_flashSize = 0;    // 0 = 見つからない
static uint8_t  g_flashId[3];
static uint8_t  g_fbuf[FRAME_MAX];  // フレーム読み出し・CRC 計算用

// ---------------- 状態 ----------------
// 状態の変更とフラッシュの読み書きは loop() だけが行う。
// BLE のイベント処理は受信パケットをキューに積むだけ。
enum : uint8_t { ST_IDLE, ST_RECEIVING, ST_PLAYING, ST_ERROR };
static uint8_t  g_state = ST_IDLE;
static uint32_t g_total = 0;
static uint32_t g_received = 0;
static uint32_t g_erasedUpTo = 0;   // ここ (フラッシュのアドレス) までは消去済み
static bool     g_nakSent = false;
static bool     g_ackPending = false, g_nakPending = false;
static int16_t  g_donePending = -1;
static uint32_t g_statDup = 0, g_statGap = 0, g_statNotifyFail = 0;
static bool     g_uiDirty = true;
static uint8_t  g_errorCode = 0;

static volatile uint8_t  g_conn = 0xff;      // 接続ハンドル (0xff = 未接続)
static volatile uint16_t g_mtu = 23;

// 再生状態
static uint16_t g_frames = 0;
static uint32_t g_framesStart = 0;  // フラッシュ上のアドレス
static uint32_t g_framePos = 0;
static uint16_t g_frameNo = 0;
static uint32_t g_nextAt = 0;

// 受信キュー (BLE タスク → loop)
// PC は 12 パケットに 1 回「応答あり」で書き、応答を待つ (docs/index.html の BATCH)。
// キューに RESP_MIN_FREE 個以上の空きがあれば、届いた時点ですぐ応答する
// (フラッシュへの書き込みを待たないので速い)。空きが少ないときだけ処理後に応答する。
#define RXQ_N         48
#define RESP_MIN_FREE 16
struct RxPkt {
  uint8_t len;
  uint8_t needResp;   // 処理後に応答を返す必要がある
  uint8_t conn;
  uint8_t data[244];
};
static RxPkt g_rxq[RXQ_N];
static volatile uint8_t g_rxHead = 0, g_rxTail = 0;
static volatile uint32_t g_rxOverflow = 0;

// GATT
static uint16_t gattdb_session_id;
static uint16_t h_generic_access, h_name, h_service, h_rx, h_tx, h_info;
static volatile bool g_txNotify = false;

static void ble_init_gatt_db();
static void ble_start_advertising();
static void ble_update_info();
static bool notify(const uint8_t *data, uint8_t len);

// ---------------- ユーティリティ ----------------
static inline uint16_t rd16(const uint8_t *p) { return p[0] | (p[1] << 8); }
static inline uint32_t rd32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static inline void wr32(uint8_t *p, uint32_t v) { p[0] = v; p[1] = v >> 8; p[2] = v >> 16; p[3] = v >> 24; }

static uint32_t crc_table[256];
static void crc_init() {
  for (uint32_t i = 0; i < 256; i++) {
    uint32_t c = i;
    for (int k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320u ^ (c >> 1)) : (c >> 1);
    crc_table[i] = c;
  }
}
static uint32_t crc32_update(uint32_t c, const uint8_t *d, uint32_t n) {
  for (uint32_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c;
}

// ---------------- フラッシュ ----------------
static inline void fl_begin() { SPI1.beginTransaction(g_flashSpi); digitalWrite(FLASH_CS, LOW); }
static inline void fl_end()   { digitalWrite(FLASH_CS, HIGH); SPI1.endTransaction(); }

static void fl_cmd(uint8_t c) { fl_begin(); SPI1.transfer(c); fl_end(); }
static void fl_cmd_addr(uint8_t c, uint32_t a) {
  SPI1.transfer(c);
  SPI1.transfer((uint8_t)(a >> 16));
  SPI1.transfer((uint8_t)(a >> 8));
  SPI1.transfer((uint8_t)a);
}
static uint8_t fl_status() {
  fl_begin();
  SPI1.transfer(0x05);
  uint8_t s = SPI1.transfer(0x00);
  fl_end();
  return s;
}
static void fl_wait(bool slow) {
  while (fl_status() & 0x01) {   // WIP (書き込み/消去中)
    if (slow) delay(1);
  }
}
static void fl_read(uint32_t a, uint8_t *buf, uint32_t n) {
  if (n == 0) return;
  fl_begin();
  fl_cmd_addr(0x03, a);
  // DMA でまとめて読む (送る中身は無視される)。
  // コアの SPI ライブラリ (5.0.0) は 2048 バイトを超える送受信を分割するとき
  // 位置をずらさない不具合があるので、2048 バイトずつに分けて呼ぶ。
  while (n > 0) {
    uint32_t k = n < 2048 ? n : 2048;
    SPI1.transfer(buf, k);
    buf += k; n -= k;
  }
  fl_end();
}
static uint32_t g_flashBusyUs = 0;   // 消去・書き込みにかかった時間 (統計用)

static void fl_erase_sector(uint32_t a) {
  uint32_t t0 = micros();
  fl_cmd(0x06);                  // 書き込み許可
  fl_begin();
  fl_cmd_addr(0x20, a);          // 4KB 消去
  fl_end();
  fl_wait(true);
  g_flashBusyUs += micros() - t0;
}
static void fl_program(uint32_t a, const uint8_t *d, uint32_t n) {
  // 送信専用の DMA 転送は受信側にデータが残り、直後のステータス読み出し
  // (書き込み完了待ち) が古い値を読んでしまうことがある。送受信の転送を使う。
  static uint8_t tmp[FLASH_PAGE];
  uint32_t t0 = micros();
  while (n > 0) {
    uint32_t room = FLASH_PAGE - (a % FLASH_PAGE);   // ページをまたがないように
    uint32_t k = n < room ? n : room;
    memcpy(tmp, d, k);
    fl_cmd(0x06);
    fl_begin();
    fl_cmd_addr(0x02, a);
    SPI1.transfer(tmp, k);
    fl_end();
    fl_wait(false);
    a += k; d += k; n -= k;
  }
  g_flashBusyUs += micros() - t0;
}

// 最後の 4KB で書き込み・読み出しを試す。結果を msg に入れて返す
static bool fl_selftest(char *msg, size_t msgLen) {
  uint8_t sr = fl_status();
  if (sr & 0x3C) {               // ブロック保護ビットが立っていたら外す
    fl_cmd(0x06);
    fl_begin(); SPI1.transfer(0x01); SPI1.transfer(0x00); fl_end();
    fl_wait(false);
  }
  uint32_t a = g_flashSize - FLASH_SECTOR;
  fl_erase_sector(a);
  uint8_t buf[300];
  fl_read(a, buf, 16);
  bool erased = true;
  for (int i = 0; i < 16; i++) if (buf[i] != 0xFF) erased = false;
  uint8_t pat[300];
  for (int i = 0; i < 300; i++) pat[i] = (uint8_t)(i * 7 + 3);
  fl_program(a + 100, pat, sizeof(pat));   // ページ境界 (256) をまたぐ
  memset(buf, 0, sizeof(buf));
  fl_read(a + 100, buf, sizeof(buf));
  int bad = 0;
  for (int i = 0; i < 300; i++) if (buf[i] != pat[i]) bad++;
  snprintf(msg, msgLen, "SR %02X->%02X erase %s rw %s (%d bad)", sr, fl_status(),
           erased ? "OK" : "NG", bad ? "NG" : "OK", bad);
  return erased && bad == 0;
}
static bool fl_init() {
  pinMode(FLASH_CS, OUTPUT);
  digitalWrite(FLASH_CS, HIGH);
  SPI1.begin();
  fl_cmd(0xAB);                  // 省電力の休止状態から起こす
  delayMicroseconds(100);
  fl_begin();
  SPI1.transfer(0x9F);           // JEDEC ID
  for (int i = 0; i < 3; i++) g_flashId[i] = SPI1.transfer(0x00);
  fl_end();
  uint8_t cap = g_flashId[2];
  if (g_flashId[0] == 0x00 || g_flashId[0] == 0xFF || cap < 0x10 || cap > 0x1A) return false;
  g_flashSize = 1u << cap;
  return true;
}
static uint32_t flash_max_bytes() { return g_flashSize ? g_flashSize - FLASH_DATA_ADDR : 0; }

// ---------------- 通知 ----------------
static bool send_ack(uint8_t code, uint32_t v) {
  uint8_t m[5] = { code };
  wr32(m + 1, v);
  return notify(m, 5);
}
static bool send_done(uint8_t result) {
  uint8_t m[2] = { 0x83, result };
  return notify(m, 2);
}
static void flush_notifications() {
  if (g_nakPending) {
    g_nakPending = false;
    if (!send_ack(0x82, g_received)) g_nakPending = true;
  }
  if (g_ackPending) {
    g_ackPending = false;
    if (!send_ack(0x81, g_received)) g_ackPending = true;
  }
  if (g_donePending >= 0) {
    int16_t d = g_donePending;
    g_donePending = -1;
    if (!send_done((uint8_t)d)) g_donePending = d;
  }
}

// ---------------- 画面 ----------------
static void screen_message(const char *l1, const char *l2, uint16_t color) {
  display.fillScreen(0x0000);
  display.setTextSize(1);
  display.setTextColor(color);
  display.setCursor(2, 4);
  display.print("BLE GIF Player");
  display.drawFastHLine(0, 14, SCR_W, 0x39E7);
  display.setTextColor(0xFFFF);
  display.setCursor(2, 22);
  display.print(l1);
  display.setCursor(2, 34);
  display.print(l2);
  g_forceFull = true;
}

static uint8_t g_progShown = 0;
static void screen_progress_init() {
  screen_message("Receiving...", "", 0x07FF);
  display.drawRect(2, 48, SCR_W - 4, 10, 0xFFFF);
  g_progShown = 0;
}
static void screen_progress_update() {
  if (g_total == 0) return;
  uint8_t pct = (uint8_t)((uint64_t)g_received * 100 / g_total);
  if (pct == g_progShown) return;
  int wFull = SCR_W - 8;
  int x0 = 4 + wFull * g_progShown / 100;
  int x1 = 4 + wFull * pct / 100;
  if (x1 > x0) display.fillRect(x0, 50, x1 - x0, 6, 0x07E0);
  display.fillRect(2, 34, SCR_W - 4, 8, 0x0000);
  display.setCursor(2, 34);
  display.setTextColor(0xFFFF);
  display.print(pct);
  display.print("%  ");
  display.print(g_received / 1024);
  display.print("KB");
  g_progShown = pct;
}

static void screen_status() {
  switch (g_state) {
    case ST_IDLE:
      if (!g_flashSize)        screen_message("Flash not found", "", 0xF800);
      else if (g_conn == 0xff) screen_message("Waiting for PC", DEVICE_NAME, 0xFFE0);
      else                     screen_message("Connected", "Select a GIF", 0x07E0);
      break;
    case ST_RECEIVING:
      screen_progress_init();
      break;
    case ST_ERROR: {
      const char *msg = g_errorCode == 1 ? "CRC error" : g_errorCode == 2 ? "Format error"
                      : g_errorCode == 3 ? "Too large" : g_errorCode == 4 ? "No flash" : "Error";
      screen_message(msg, "Send again", 0xF800);
      break;
    }
    default:
      break;
  }
}

// ---------------- アニメ ----------------
// フラッシュ上のデータの CRC を計算する
static uint32_t flash_crc(uint32_t addr, uint32_t n) {
  uint32_t c = 0xFFFFFFFFu;
  while (n > 0) {
    uint32_t k = n < sizeof(g_fbuf) ? n : sizeof(g_fbuf);
    fl_read(addr, g_fbuf, k);
    c = crc32_update(c, g_fbuf, k);
    addr += k; n -= k;
  }
  return c ^ 0xFFFFFFFFu;
}

// フラッシュ上のアニメの形式を確かめ、パレットと再生位置を準備する
static uint8_t parse_anim(uint32_t total) {
  uint8_t h[12];
  if (total < 12) return 2;
  fl_read(FLASH_DATA_ADDR, h, 12);
  if (memcmp(h, "GA01", 4) != 0 || h[4] != SCR_W || h[5] != SCR_H) return 2;
  uint16_t frames = rd16(h + 6);
  uint16_t ncol = rd16(h + 8);
  if (frames == 0 || ncol == 0 || ncol > 256) return 2;
  uint32_t pos = 12 + (uint32_t)ncol * 2;
  if (pos > total) return 2;
  uint8_t pb[512];
  fl_read(FLASH_DATA_ADDR + 12, pb, ncol * 2);
  memset(g_pal, 0, sizeof(g_pal));
  for (uint16_t i = 0; i < ncol; i++) g_pal[i] = rd16(pb + i * 2);
  uint32_t start = pos;
  for (uint16_t f = 0; f < frames; f++) {
    if (pos + 4 > total) return 2;
    uint8_t fh[4];
    fl_read(FLASH_DATA_ADDR + pos, fh, 4);
    uint16_t len = rd16(fh + 2);
    if (len > FRAME_MAX) return 2;
    pos += 4 + len;
  }
  if (pos != total) return 2;
  g_total = total;
  g_frames = frames;
  g_framesStart = FLASH_DATA_ADDR + start;
  g_framePos = g_framesStart;
  g_frameNo = 0;
  g_nextAt = millis();
  g_forceFull = true;
  return 0;
}

// 起動時: 保存されているアニメがあれば再生する
static bool load_saved_anim() {
  uint8_t h[12];
  fl_read(FLASH_HDR_ADDR, h, sizeof(h));
  if (memcmp(h, "GAF1", 4) != 0) return false;
  uint32_t total = rd32(h + 4), crc = rd32(h + 8);
  if (total == 0 || total > flash_max_bytes()) return false;
  screen_message("Loading...", "", 0x07FF);
  uint32_t t0 = millis();
  if (flash_crc(FLASH_DATA_ADDR, total) != crc) { Serial.println("saved anim: CRC NG"); return false; }
  if (parse_anim(total) != 0) { Serial.println("saved anim: format NG"); return false; }
  Serial.printf("saved anim: %lu bytes, %u frames (check %lu ms)\n",
                (unsigned long)total, g_frames, (unsigned long)(millis() - t0));
  return true;
}

// 差分データを g_idx に展開し、書き換わった画素範囲を返す
static void decode_frame(const uint8_t *d, uint32_t len, int32_t &pmin, int32_t &pmax) {
  uint32_t i = 0, p = 0;
  while (i < len && p < NPIX) {
    uint8_t b = d[i++];
    uint32_t n = (b & 0x3F) + 1;
    switch (b >> 6) {
      case 0:  // SKIP
        p += n;
        break;
      case 3:  // 長いSKIP
        if (i >= len) return;
        p += (((uint32_t)(b & 0x3F) << 8) | d[i++]) + 1;
        break;
      case 1: {  // RUN
        if (i >= len) return;
        uint8_t v = d[i++];
        if (p + n > NPIX) n = NPIX - p;
        memset(g_idx + p, v, n);
        if ((int32_t)p < pmin) pmin = p;
        if ((int32_t)(p + n - 1) > pmax) pmax = p + n - 1;
        p += n;
        break;
      }
      case 2: {  // LITERAL
        if (i + n > len) n = len - i;
        if (p + n > NPIX) n = NPIX - p;
        memcpy(g_idx + p, d + i, n);
        if ((int32_t)p < pmin) pmin = p;
        if ((int32_t)(p + n - 1) > pmax) pmax = p + n - 1;
        i += n;
        p += n;
        break;
      }
    }
  }
}

#if USE_HW_SPI
// 1 帯 (96x8) 分の送信バッファ。RGB565 をビッグエンディアンで詰める。
// このコアの SPI は 1 回の転送ごとに DMA を起動するので、1 画素ずつ送ると
// 1 画素あたり約 26us かかる。まとめて 1 回で送れば SPI の速度どおりになる。
static uint8_t g_tx[SCR_W * TILE * 2];
#endif

// g_idx と g_shown を比べて、変化した 8x8 タイルを含む部分だけを OLED に送る。
// 8 行の帯ごとに「左端の変化タイル〜右端の変化タイル」を 1 つの矩形にする。
// 戻り値は送った画素数。
static uint32_t push_dirty_tiles(int y0, int y1, bool full) {
  uint32_t pixels = 0;
  int ty0 = y0 / TILE, ty1 = y1 / TILE;
  display.startWrite();
  for (int ty = ty0; ty <= ty1; ty++) {
    int txMin = TILES_X, txMax = -1;
    for (int tx = 0; tx < TILES_X; tx++) {
      bool d = full;
      for (int y = ty * TILE; !d && y < ty * TILE + TILE; y++) {
        int o = y * SCR_W + tx * TILE;
        d = memcmp(g_idx + o, g_shown + o, TILE) != 0;
      }
      if (d) {
        if (tx < txMin) txMin = tx;
        txMax = tx;
      }
    }
    if (txMax < 0) continue;
    int x0 = txMin * TILE, w = (txMax - txMin + 1) * TILE;
    display.setAddrWindow(x0, ty * TILE, w, TILE);   // 終わると DC=データ になっている
#if USE_HW_SPI
    uint8_t *q = g_tx;
    for (int y = ty * TILE; y < ty * TILE + TILE; y++) {
      int o = y * SCR_W + x0;
      for (int x = 0; x < w; x++) {
        uint16_t c = g_pal[g_idx[o + x]];
        *q++ = c >> 8;
        *q++ = c & 0xFF;
      }
      memcpy(g_shown + o, g_idx + o, w);
    }
    SPI.transfer(g_tx, (size_t)(q - g_tx), true);   // 送信のみ・完了まで待つ
#else
    for (int y = ty * TILE; y < ty * TILE + TILE; y++) {
      int o = y * SCR_W + x0;
      for (int x = 0; x < w; x++) g_line[x] = g_pal[g_idx[o + x]];
      memcpy(g_shown + o, g_idx + o, w);
      display.writePixels(g_line, w);
    }
#endif
    pixels += (uint32_t)w * TILE;
  }
  display.endWrite();
  return pixels;
}

static void play_next_frame() {
  static uint32_t statFrames = 0, statPixels = 0, statUs = 0, statReadUs = 0, statAt = 0, statLate = 0, statMaxLate = 0;
  uint32_t late = millis() - g_nextAt;   // 予定時刻からの遅れ
  if (late > statMaxLate) statMaxLate = late;
  if (late > 10) statLate++;

  if (g_frameNo == 0) {
    memset(g_idx, 0, sizeof(g_idx));
    g_framePos = g_framesStart;
  }
  uint32_t tr = micros();
  uint8_t fh[4];
  fl_read(g_framePos, fh, 4);
  uint16_t delayMs = rd16(fh);
  uint16_t len = rd16(fh + 2);
  if (len > FRAME_MAX) { g_frameNo = 0; return; }
  fl_read(g_framePos + 4, g_fbuf, len);
  statReadUs += micros() - tr;

  int32_t pmin = NPIX, pmax = -1;
  decode_frame(g_fbuf, len, pmin, pmax);
  // ループ先頭は「全画素0から」展開し直すので、範囲は画面全体と比べる
  if (g_frameNo == 0 || g_forceFull) { pmin = 0; pmax = NPIX - 1; }
  if (pmax >= pmin) {
    uint32_t t0 = micros();
    statPixels += push_dirty_tiles(pmin / SCR_W, pmax / SCR_W, g_forceFull);
    statUs += micros() - t0;
    g_forceFull = false;
  }
  if (++statFrames >= 50 || millis() - statAt > 5000) {
    // ns/px: 1画素を送るのにかかった時間。SPI 8MHz の理論値は 16bit = 2000ns
    Serial.printf("draw: %lu frames, avg %lu px / %lu us draw / %lu us flash read, %lu ns/px, late>10ms %lu, max late %lu ms\n",
                  (unsigned long)statFrames, (unsigned long)(statPixels / statFrames),
                  (unsigned long)(statUs / statFrames), (unsigned long)(statReadUs / statFrames),
                  (unsigned long)(statPixels ? (uint64_t)statUs * 1000 / statPixels : 0),
                  (unsigned long)statLate, (unsigned long)statMaxLate);
    statFrames = statPixels = statUs = statReadUs = statLate = statMaxLate = 0;
    statAt = millis();
  }

  g_framePos += 4 + len;
  if (++g_frameNo >= g_frames) g_frameNo = 0;

  if (delayMs < 20) delayMs = 20;
  g_nextAt += delayMs;
  uint32_t now = millis();
  if ((int32_t)(now - g_nextAt) > 200) g_nextAt = now;  // 大きく遅れたら追いつくのをやめる
}

// ---------------- 受信処理 (loop から呼ぶ) ----------------
static void set_state(uint8_t st) {
  g_state = st;
  g_uiDirty = true;
}

static void handle_packet(const uint8_t *d, uint8_t len) {
  if (len == 0) return;
  switch (d[0]) {
    case 0x01: {  // BEGIN
      if (len < 5) return;
      uint32_t total = rd32(d + 1);
      if (!g_flashSize) { g_errorCode = 4; set_state(ST_ERROR); g_donePending = 4; return; }
      if (total == 0 || total > flash_max_bytes()) {
        g_errorCode = 3; set_state(ST_ERROR); g_donePending = 3; return;
      }
      fl_erase_sector(FLASH_HDR_ADDR);   // 保存済みのアニメを無効にする
      g_total = total;
      g_received = 0;
      g_erasedUpTo = FLASH_DATA_ADDR;
      g_nakSent = false;
      g_nakPending = false;
      g_statDup = g_statGap = g_statNotifyFail = 0;
      g_rxOverflow = 0;
      set_state(ST_RECEIVING);
      if (!send_ack(0x81, 0)) g_ackPending = true;
      break;
    }
    case 0x02: {  // DATA
      if (len < 5 || g_state != ST_RECEIVING) return;
      uint32_t off = rd32(d + 1);
      uint32_t n = len - 5;
      if (off < g_received) { g_statDup++; return; }   // 受信済みの再送 → 捨てる
      if (off > g_received) {                          // 抜けがあった → 送り直してと頼む
        g_statGap++;
        if (!g_nakSent) {
          g_nakSent = true;
          if (!send_ack(0x82, g_received)) g_nakPending = true;
        }
        return;
      }
      if (off + n > g_total) n = g_total - off;
      uint32_t a = FLASH_DATA_ADDR + off;
      while (a + n > g_erasedUpTo) {                   // 書く前に 4KB ずつ消去
        fl_erase_sector(g_erasedUpTo);
        g_erasedUpTo += FLASH_SECTOR;
      }
      fl_program(a, d + 5, n);
      g_received = off + n;
      g_nakSent = false;
      break;
    }
    case 0x03: {  // END
      if (len < 5 || g_state != ST_RECEIVING) return;
      if (g_received != g_total) {
        if (!send_ack(0x82, g_received)) g_nakPending = true;
        return;
      }
      uint32_t crc = rd32(d + 1);
      screen_message("Checking...", "", 0x07FF);
      uint32_t t0 = millis();
      uint8_t r = (flash_crc(FLASH_DATA_ADDR, g_total) == crc) ? 0 : 1;
      if (r) {
        uint8_t h[16];
        fl_read(FLASH_DATA_ADDR, h, 16);
        Serial.printf("CRC NG. head: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                      h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
      }
      if (r == 0) r = parse_anim(g_total);
      Serial.printf("verify: %u (%lu bytes, %u frames, %lu ms)\n", r, (unsigned long)g_total,
                    g_frames, (unsigned long)(millis() - t0));
      if (r == 0) {
        uint8_t h[12] = { 'G', 'A', 'F', '1' };
        wr32(h + 4, g_total);
        wr32(h + 8, crc);
        fl_program(FLASH_HDR_ADDR, h, sizeof(h));     // これで電源を切っても残る
        g_state = ST_PLAYING;
      } else {
        g_errorCode = r;
        set_state(ST_ERROR);
      }
      g_donePending = r;
      break;
    }
    case 0x04:  // STOP
      set_state(ST_IDLE);
      break;
  }
}

static void process_rx_queue() {
  while (g_rxTail != g_rxHead) {
    RxPkt &p = g_rxq[g_rxTail];
    handle_packet(p.data, p.len);
    if (p.needResp) sl_bt_gatt_server_send_user_write_response(p.conn, h_rx, 0);
    g_rxTail = (g_rxTail + 1) % RXQ_N;
  }
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  crc_init();
  display.begin(SPI_HZ);
  display.fillScreen(0x0000);

  if (fl_init()) {
    char msg[64];
    bool ok = fl_selftest(msg, sizeof(msg));
    Serial.printf("flash: id %02X %02X %02X, %lu KB, selftest %s: %s\n", g_flashId[0], g_flashId[1],
                  g_flashId[2], (unsigned long)(g_flashSize / 1024), ok ? "OK" : "NG", msg);
    char l1[24];
    snprintf(l1, sizeof(l1), "Flash %luKB %s", (unsigned long)(g_flashSize / 1024), ok ? "OK" : "NG");
    screen_message(l1, ok ? "" : "RW test failed", ok ? 0x07E0 : 0xF800);
    delay(ok ? 800 : 5000);
    if (load_saved_anim()) g_state = ST_PLAYING;
  } else {
    Serial.printf("flash: not found (id %02X %02X %02X)\n", g_flashId[0], g_flashId[1], g_flashId[2]);
  }
  ble_update_info();
  g_uiDirty = true;
}

void loop() {
  static uint8_t shownState = 0xff;
  static uint8_t shownConn = 0xfe;

  process_rx_queue();
  flush_notifications();

  // 受信中に切断されたら待ち受けに戻る
  if (g_state == ST_RECEIVING && g_conn == 0xff) set_state(ST_IDLE);

  if (g_state == ST_RECEIVING) {
    // 1 秒ごとに: 受信速度と、そのうちフラッシュの消去・書き込みに使った時間
    static uint32_t lastStat = 0, lastRx = 0, lastBusy = 0;
    uint32_t now = millis();
    if (now - lastStat >= 1000) {
      if (g_received < lastRx) lastRx = 0;
      Serial.printf("rx %lu/%lu  %lu B/s  flash busy %lu ms  dup %lu gap %lu overflow %lu notifyFail %lu\n",
                    (unsigned long)g_received, (unsigned long)g_total,
                    (unsigned long)((uint64_t)(g_received - lastRx) * 1000 / (now - lastStat)),
                    (unsigned long)((g_flashBusyUs - lastBusy) / 1000),
                    (unsigned long)g_statDup, (unsigned long)g_statGap, (unsigned long)g_rxOverflow,
                    (unsigned long)g_statNotifyFail);
      lastStat = now;
      lastRx = g_received;
      lastBusy = g_flashBusyUs;
    }
  }
  uint8_t conn = g_conn;
  if (g_uiDirty || g_state != shownState || (g_state == ST_IDLE && conn != shownConn)) {
    g_uiDirty = false;
    shownState = g_state;
    shownConn = conn;
    screen_status();
  }

  if (g_state == ST_RECEIVING) {
    screen_progress_update();
  } else if (g_state == ST_PLAYING) {
    if ((int32_t)(millis() - g_nextAt) >= 0) play_next_frame();
  }
  if (g_rxTail == g_rxHead) delay(1);
}

// ---------------- BLE ----------------
void sl_bt_on_event(sl_bt_msg_t *evt) {
  switch (SL_BT_MSG_ID(evt->header)) {
    case sl_bt_evt_system_boot_id:
      Serial.println("BLE stack booted");
      ble_init_gatt_db();
      ble_start_advertising();
      break;

    case sl_bt_evt_connection_opened_id: {
      uint8_t c = evt->data.evt_connection_opened.connection;
      g_conn = c;
      g_mtu = 23;
      ble_update_info();
      // 短い接続間隔 (7.5〜15ms)、2M PHY、長いリンク層パケット (251 バイト) を要求する。
      // 実測 (Windows + Chrome): 15ms・1M PHY になり約 14KB/s。7.5ms 固定にしても
      // Chrome の 1 回の書き込みに 17〜20ms かかるので速くならず、むしろ少し遅かった。
      sl_bt_connection_set_parameters(c, 6, 12, 0, 400, 0, 0xffff);
      sl_bt_connection_set_preferred_phy(c, 0x02, 0xff);
      sl_bt_connection_set_data_length(c, 251, 2120);
      Serial.println("connected");
      break;
    }

    case sl_bt_evt_connection_closed_id:
      Serial.println("disconnected");
      g_conn = 0xff;
      g_txNotify = false;
      ble_start_advertising();
      break;

    // 転送速度を左右する接続条件 (PC 側が実際に受け入れた値) を表示する
    case sl_bt_evt_connection_parameters_id:
      Serial.printf("conn interval %u.%02u ms, latency %u, timeout %u ms\n",
                    evt->data.evt_connection_parameters.interval * 125 / 100,
                    evt->data.evt_connection_parameters.interval * 125 % 100,
                    evt->data.evt_connection_parameters.latency,
                    evt->data.evt_connection_parameters.timeout * 10);
      break;
    case sl_bt_evt_connection_phy_status_id:
      Serial.printf("PHY %s\n", evt->data.evt_connection_phy_status.phy == 2 ? "2M" :
                                evt->data.evt_connection_phy_status.phy == 1 ? "1M" : "coded");
      break;
    case sl_bt_evt_connection_data_length_id:
      Serial.printf("data length tx %u / rx %u bytes\n",
                    evt->data.evt_connection_data_length.tx_data_len,
                    evt->data.evt_connection_data_length.rx_data_len);
      break;

    case sl_bt_evt_gatt_mtu_exchanged_id:
      g_mtu = evt->data.evt_gatt_mtu_exchanged.mtu;
      ble_update_info();
      Serial.printf("MTU %u\n", g_mtu);
      break;

    case sl_bt_evt_gatt_server_characteristic_status_id: {
      sl_bt_evt_gatt_server_characteristic_status_t &cs = evt->data.evt_gatt_server_characteristic_status;
      if (cs.characteristic == h_tx && cs.status_flags == sl_bt_gatt_server_client_config) {
        g_txNotify = (cs.client_config_flags & sl_bt_gatt_notification) != 0;
      }
      break;
    }

    // RX はアプリ管理の値なので、書き込みはこのイベントで届く。ここではキューに積むだけ。
    // 応答ありの書き込みには、キューに余裕があればすぐ、なければ loop() が処理後に応答する。
    case sl_bt_evt_gatt_server_user_write_request_id: {
      sl_bt_evt_gatt_server_user_write_request_t &w = evt->data.evt_gatt_server_user_write_request;
      bool needResp = (w.att_opcode == sl_bt_gatt_write_request);
      if (w.characteristic != h_rx) {
        if (needResp) sl_bt_gatt_server_send_user_write_response(w.connection, w.characteristic, 0x0E);
        break;
      }
      uint8_t next = (g_rxHead + 1) % RXQ_N;
      if (next == g_rxTail) {          // キューが一杯 (通常は起きない) → 捨てる。抜けは NAK で回復
        g_rxOverflow++;
        if (needResp) sl_bt_gatt_server_send_user_write_response(w.connection, h_rx, 0);
        break;
      }
      RxPkt &p = g_rxq[g_rxHead];
      p.len = w.value.len > sizeof(p.data) ? sizeof(p.data) : w.value.len;
      memcpy(p.data, w.value.data, p.len);
      uint8_t used = (uint8_t)((next + RXQ_N - g_rxTail) % RXQ_N);
      bool respondNow = needResp && (RXQ_N - 1 - used) >= RESP_MIN_FREE;
      p.needResp = needResp && !respondNow;
      p.conn = w.connection;
      g_rxHead = next;
      if (respondNow) sl_bt_gatt_server_send_user_write_response(w.connection, h_rx, 0);
      break;
    }

    default:
      break;
  }
}

static bool notify(const uint8_t *data, uint8_t len) {
  if (g_conn == 0xff || !g_txNotify) return true;   // 送り先がないので捨てる
  sl_status_t sc = sl_bt_gatt_server_send_notification(g_conn, h_tx, len, data);
  if (sc != SL_STATUS_OK) { g_statNotifyFail++; return false; }
  return true;
}

static void ble_update_info() {
  if (!h_info) return;
  uint8_t v[9];
  v[0] = PROTO_VER;
  v[1] = g_mtu & 0xff;
  v[2] = g_mtu >> 8;
  wr32(v + 3, flash_max_bytes());
  v[7] = SCR_W;
  v[8] = SCR_H;
  sl_bt_gatt_server_write_attribute_value(h_info, 0, sizeof(v), v);
}

static void ble_start_advertising() {
  static uint8_t adv = 0xff;
  sl_status_t sc;
  if (adv == 0xff) {
    sc = sl_bt_advertiser_create_set(&adv);
    app_assert_status(sc);
    sc = sl_bt_advertiser_set_timing(adv, 160, 160, 0, 0);  // 100ms
    app_assert_status(sc);
  }
  sc = sl_bt_legacy_advertiser_generate_data(adv, sl_bt_advertiser_general_discoverable);
  app_assert_status(sc);
  sc = sl_bt_legacy_advertiser_start(adv, sl_bt_advertiser_connectable_scannable);
  app_assert_status(sc);
}

// 7e4a10xx-5c3b-4f6e-9a21-3d8b6c0f1a20 (リトルエンディアン)
static uuid_128 make_uuid(uint16_t id) {
  uuid_128 u = { .data = { 0x20, 0x1a, 0x0f, 0x6c, 0x8b, 0x3d, 0x21, 0x9a,
                           0x6e, 0x4f, 0x3b, 0x5c, 0x00, 0x10, 0x4a, 0x7e } };
  u.data[12] = id & 0xff;
  u.data[13] = id >> 8;
  return u;
}

static void ble_init_gatt_db() {
  sl_status_t sc;
  sc = sl_bt_gattdb_new_session(&gattdb_session_id);
  app_assert_status(sc);

  // Generic Access (デバイス名)
  const uint8_t ga_uuid[] = { 0x00, 0x18 };
  sc = sl_bt_gattdb_add_service(gattdb_session_id, sl_bt_gattdb_primary_service,
                                SL_BT_GATTDB_ADVERTISED_SERVICE, sizeof(ga_uuid), ga_uuid,
                                &h_generic_access);
  app_assert_status(sc);
  const sl_bt_uuid_16_t name_uuid = { .data = { 0x00, 0x2A } };
  sc = sl_bt_gattdb_add_uuid16_characteristic(gattdb_session_id, h_generic_access,
                                              SL_BT_GATTDB_CHARACTERISTIC_READ, 0, 0, name_uuid,
                                              sl_bt_gattdb_fixed_length_value,
                                              sizeof(DEVICE_NAME) - 1, sizeof(DEVICE_NAME) - 1,
                                              (const uint8_t *)DEVICE_NAME, &h_name);
  app_assert_status(sc);
  sc = sl_bt_gattdb_start_service(gattdb_session_id, h_generic_access);
  app_assert_status(sc);

  // GIF サービス
  uuid_128 svc = make_uuid(0x1000);
  sc = sl_bt_gattdb_add_service(gattdb_session_id, sl_bt_gattdb_primary_service,
                                0, sizeof(svc), svc.data, &h_service);
  app_assert_status(sc);

  uint8_t zero[9] = { 0 };
  // RX: アプリ管理の値 (書き込みは user_write_request イベントで届き、応答はアプリが返す)
  sc = sl_bt_gattdb_add_uuid128_characteristic(gattdb_session_id, h_service,
                                               SL_BT_GATTDB_CHARACTERISTIC_WRITE | SL_BT_GATTDB_CHARACTERISTIC_WRITE_NO_RESPONSE,
                                               0, 0, make_uuid(0x1001),
                                               sl_bt_gattdb_user_managed_value, 244, 0, zero, &h_rx);
  app_assert_status(sc);
  sc = sl_bt_gattdb_add_uuid128_characteristic(gattdb_session_id, h_service,
                                               SL_BT_GATTDB_CHARACTERISTIC_NOTIFY,
                                               0, 0, make_uuid(0x1002),
                                               sl_bt_gattdb_variable_length_value, 20, 1, zero, &h_tx);
  app_assert_status(sc);
  sc = sl_bt_gattdb_add_uuid128_characteristic(gattdb_session_id, h_service,
                                               SL_BT_GATTDB_CHARACTERISTIC_READ,
                                               0, 0, make_uuid(0x1003),
                                               sl_bt_gattdb_fixed_length_value, 9, 9, zero, &h_info);
  app_assert_status(sc);
  sc = sl_bt_gattdb_start_service(gattdb_session_id, h_service);
  app_assert_status(sc);

  sc = sl_bt_gattdb_commit(gattdb_session_id);
  app_assert_status(sc);
  ble_update_info();
}
