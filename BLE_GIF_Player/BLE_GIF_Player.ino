/*
 * XIAO MG24 BLE GIF Player
 *
 * PC のブラウザ (docs/index.html, Web Bluetooth) から送られてきた
 * GIF アニメ (96x64 / 256色に変換済み) を受信し、SSD1331 OLED でループ再生する。
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
 *          次を送る (これが流量制御。XIAO が処理し終わるまで次が来ない)
 *   TX   ...1002  Notify (XIAO → PC)
 *          0x81 ACK    u32 receivedBytes    (BEGIN の受付時)
 *          0x82 NAK    u32 expectedOffset   (抜けを検出。この位置から送り直して)
 *          0x83 DONE   u8 result (0=OK 1=CRC 2=FORMAT 3=TOO_LARGE)
 *   INFO ...1003  Read: u8 ver, u16 mtu, u32 bufSize, u8 width, u8 height
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
 */
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
// 画面が映らないときだけ 0 に戻す
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

// ---------------- 受信バッファ ----------------
#define BUF_SIZE   (160u * 1024u)
#define PROTO_VER  1
static const char DEVICE_NAME[] = "XIAO-GIF";

static uint8_t  g_buf[BUF_SIZE];
static uint8_t  g_idx[NPIX];          // 現在のフレーム (パレット番号)
static uint8_t  g_shown[NPIX];        // いま画面に出ている内容 (差分描画用)
static bool     g_forceFull = true;   // 次のフレームは全画面を描く
static uint16_t g_pal[256];
static uint16_t g_line[SCR_W];

// 差分描画: 画面を 8x8 のタイルに分け、変化したタイルだけ送る
#define TILE   8
#define TILES_X (SCR_W / TILE)
#define TILES_Y (SCR_H / TILE)

enum : uint8_t { ST_IDLE, ST_RECEIVING, ST_VERIFY, ST_PLAYING, ST_ERROR };
static volatile uint8_t  g_state = ST_IDLE;
static volatile uint32_t g_total = 0;
static volatile uint32_t g_received = 0;
static volatile uint32_t g_crcExpected = 0;
static volatile bool     g_nakSent = false;
// ACK/NAK は BLE タスクで直接送らず loop() から送る。送信バッファが一杯で
// 失敗したときは次の loop() で送り直す (取りこぼすと PC 側が止まるため)
static volatile bool     g_ackPending = false;
static volatile bool     g_nakPending = false;
static volatile int16_t  g_donePending = -1;
static volatile uint32_t g_statDup = 0, g_statGap = 0, g_statNotifyFail = 0;
static volatile bool     g_uiDirty = true;
static volatile uint8_t  g_conn = 0xff;      // 接続ハンドル (0xff = 未接続)
static volatile uint16_t g_mtu = 23;
static volatile uint8_t  g_errorCode = 0;

// 再生状態 (loop タスクのみが触る)
static uint16_t g_frames = 0;
static uint32_t g_framesStart = 0;
static uint32_t g_framePos = 0;
static uint16_t g_frameNo = 0;
static uint32_t g_nextAt = 0;

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
static uint32_t crc32(const uint8_t *d, uint32_t n) {
  uint32_t c = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < n; i++) c = crc_table[(c ^ d[i]) & 0xFF] ^ (c >> 8);
  return c ^ 0xFFFFFFFFu;
}

static bool send_ack(uint8_t code, uint32_t v) {
  uint8_t m[5] = { code };
  wr32(m + 1, v);
  return notify(m, 5);
}
static bool send_done(uint8_t result) {
  uint8_t m[2] = { 0x83, result };
  return notify(m, 2);
}

// loop() から呼ぶ: 溜まっている通知を送る
static void flush_notifications() {
  if (g_nakPending) {
    g_nakPending = false;
    if (!send_ack(0x82, g_received)) g_nakPending = true;
  }
  if (g_ackPending) {
    g_ackPending = false;
    if (!send_ack(0x81, g_received)) g_ackPending = true;
  }
  int16_t d = g_donePending;
  if (d >= 0) {
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
}

static uint8_t g_progShown = 0;
static void screen_progress_init() {
  screen_message("Receiving...", "", 0x07FF);
  display.drawRect(2, 48, SCR_W - 4, 10, 0xFFFF);
  g_progShown = 0;
}
static void screen_progress_update() {
  uint32_t tot = g_total, rec = g_received;
  if (tot == 0) return;
  uint8_t pct = (uint8_t)((uint64_t)rec * 100 / tot);
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
  display.print(rec / 1024);
  display.print("KB");
  g_progShown = pct;
}

static void screen_status() {
  switch (g_state) {
    case ST_IDLE:
      if (g_conn == 0xff) screen_message("Waiting for PC", DEVICE_NAME, 0xFFE0);
      else                screen_message("Connected", "Select a GIF", 0x07E0);
      break;
    case ST_RECEIVING:
      screen_progress_init();
      break;
    case ST_ERROR: {
      const char *msg = g_errorCode == 1 ? "CRC error" : g_errorCode == 2 ? "Format error"
                      : g_errorCode == 3 ? "Too large" : "Error";
      screen_message(msg, "Send again", 0xF800);
      break;
    }
    default:
      break;
  }
}

// ---------------- アニメ ----------------
static uint8_t verify_and_parse() {
  uint32_t total = g_total;
  if (crc32(g_buf, total) != g_crcExpected) return 1;
  if (total < 12 || memcmp(g_buf, "GA01", 4) != 0) return 2;
  if (g_buf[4] != SCR_W || g_buf[5] != SCR_H) return 2;
  uint16_t frames = rd16(g_buf + 6);
  uint16_t ncol = rd16(g_buf + 8);
  if (frames == 0 || ncol == 0 || ncol > 256) return 2;
  uint32_t pos = 12 + (uint32_t)ncol * 2;
  if (pos > total) return 2;
  memset(g_pal, 0, sizeof(g_pal));
  for (uint16_t i = 0; i < ncol; i++) g_pal[i] = rd16(g_buf + 12 + i * 2);
  uint32_t start = pos;
  for (uint16_t f = 0; f < frames; f++) {
    if (pos + 4 > total) return 2;
    pos += 4 + rd16(g_buf + pos + 2);
  }
  if (pos != total) return 2;
  g_frames = frames;
  g_framesStart = start;
  g_framePos = start;
  g_frameNo = 0;
  g_nextAt = millis();
  g_forceFull = true;   // 画面には受信中の表示が出ているので最初は全部描く
  return 0;
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

// g_idx と g_shown を比べて、変化した 8x8 タイルだけを OLED に送る。
// 横に並んだ変化タイルはまとめて 1 つの矩形として送る。戻り値は送った画素数。
static uint32_t push_dirty_tiles(int y0, int y1, bool full) {
  uint32_t pixels = 0;
  int ty0 = y0 / TILE, ty1 = y1 / TILE;
  display.startWrite();
  for (int ty = ty0; ty <= ty1; ty++) {
    bool dirty[TILES_X];
    for (int tx = 0; tx < TILES_X; tx++) {
      bool d = full;
      for (int y = ty * TILE; !d && y < ty * TILE + TILE; y++) {
        int o = y * SCR_W + tx * TILE;
        d = memcmp(g_idx + o, g_shown + o, TILE) != 0;
      }
      dirty[tx] = d;
    }
    for (int tx = 0; tx < TILES_X;) {
      if (!dirty[tx]) { tx++; continue; }
      int tx1 = tx;
      while (tx1 + 1 < TILES_X && dirty[tx1 + 1]) tx1++;
      int x0 = tx * TILE, w = (tx1 - tx + 1) * TILE;
      display.setAddrWindow(x0, ty * TILE, w, TILE);
      for (int y = ty * TILE; y < ty * TILE + TILE; y++) {
        int o = y * SCR_W + x0;
        for (int x = 0; x < w; x++) g_line[x] = g_pal[g_idx[o + x]];
        memcpy(g_shown + o, g_idx + o, w);
        display.writePixels(g_line, w);
      }
      pixels += (uint32_t)w * TILE;
      tx = tx1 + 1;
    }
  }
  display.endWrite();
  return pixels;
}

static void play_next_frame() {
  uint32_t total = g_total;
  if (g_frameNo == 0) {
    memset(g_idx, 0, sizeof(g_idx));
    g_framePos = g_framesStart;
  }
  if (g_framePos + 4 > total) { g_frameNo = 0; return; }
  uint16_t delayMs = rd16(g_buf + g_framePos);
  uint16_t len = rd16(g_buf + g_framePos + 2);
  if (g_framePos + 4 + len > total) { g_frameNo = 0; return; }

  int32_t pmin = NPIX, pmax = -1;
  decode_frame(g_buf + g_framePos + 4, len, pmin, pmax);
  // ループ先頭は「全画素0から」展開し直すので、範囲は画面全体と比べる
  if (g_frameNo == 0 || g_forceFull) { pmin = 0; pmax = NPIX - 1; }
  if (g_state != ST_PLAYING) return;                    // 受信が始まったら描かない

  static uint32_t statFrames = 0, statPixels = 0, statUs = 0, statAt = 0, statLate = 0, statMaxLate = 0;
  // 予定時刻からどれだけ遅れて描き始めたか (描画が間に合っていれば 0〜数ms)
  uint32_t late = millis() - g_nextAt;
  if (late > statMaxLate) statMaxLate = late;
  if (late > 10) statLate++;
  if (pmax >= pmin) {
    uint32_t t0 = micros();
    statPixels += push_dirty_tiles(pmin / SCR_W, pmax / SCR_W, g_forceFull);
    statUs += micros() - t0;
    g_forceFull = false;
  }
  if (++statFrames >= 50 || millis() - statAt > 5000) {
    // ns/px: 1画素を送るのにかかった時間。SPI 8MHz の理論値は 16bit = 2000ns
    Serial.printf("draw: %lu frames, avg %lu px / %lu us per frame, %lu ns/px, late>10ms %lu, max late %lu ms\n",
                  (unsigned long)statFrames, (unsigned long)(statPixels / statFrames),
                  (unsigned long)(statUs / statFrames),
                  (unsigned long)(statPixels ? (uint64_t)statUs * 1000 / statPixels : 0),
                  (unsigned long)statLate, (unsigned long)statMaxLate);
    statFrames = statPixels = statUs = statLate = statMaxLate = 0;
    statAt = millis();
  }

  g_framePos += 4 + len;
  if (++g_frameNo >= g_frames) g_frameNo = 0;

  if (delayMs < 20) delayMs = 20;
  g_nextAt += delayMs;
  uint32_t now = millis();
  if ((int32_t)(now - g_nextAt) > 200) g_nextAt = now;  // 大きく遅れたら追いつくのをやめる
}

// ---------------- Arduino ----------------
void setup() {
  Serial.begin(115200);
  crc_init();
  display.begin(SPI_HZ);
  display.fillScreen(0x0000);
  g_uiDirty = true;
}

void loop() {
  static uint8_t shownState = 0xff;
  static uint8_t shownConn = 0xfe;

  flush_notifications();

  uint8_t st = g_state;
  if (st == ST_RECEIVING) {
    static uint32_t lastStat = 0;
    if (millis() - lastStat >= 1000) {
      lastStat = millis();
      Serial.printf("rx %lu/%lu  dup %lu gap %lu notifyFail %lu\n",
                    (unsigned long)g_received, (unsigned long)g_total,
                    (unsigned long)g_statDup, (unsigned long)g_statGap, (unsigned long)g_statNotifyFail);
    }
  }
  if (g_uiDirty || st != shownState || (st == ST_IDLE && g_conn != shownConn)) {
    g_uiDirty = false;
    shownState = st;
    shownConn = g_conn;
    screen_status();
  }

  if (st == ST_RECEIVING) {
    screen_progress_update();
  } else if (st == ST_VERIFY) {
    uint8_t r = verify_and_parse();
    Serial.printf("verify: %u (%lu bytes, %u frames)\n", r, (unsigned long)g_total, g_frames);
    if (r == 0) {
      g_state = ST_PLAYING;
      shownState = ST_PLAYING;
    } else {
      g_errorCode = r;
      g_state = ST_ERROR;
    }
    g_donePending = r;
  } else if (st == ST_PLAYING) {
    if ((int32_t)(millis() - g_nextAt) >= 0) play_next_frame();
  }
  delay(1);
}

// ---------------- BLE ----------------
static void on_rx(const uint8_t *d, uint8_t len) {
  if (len == 0) return;
  switch (d[0]) {
    case 0x01: {  // BEGIN
      if (len < 5) return;
      uint32_t total = rd32(d + 1);
      if (total == 0 || total > BUF_SIZE) {
        g_errorCode = 3;
        g_state = ST_ERROR;
        g_donePending = 3;
        return;
      }
      g_state = ST_RECEIVING;
      g_total = total;
      g_received = 0;
      g_nakSent = false;
      g_nakPending = false;
      g_statDup = g_statGap = g_statNotifyFail = 0;
      g_uiDirty = true;
      if (!send_ack(0x81, 0)) g_ackPending = true;
      break;
    }
    case 0x02: {  // DATA
      if (len < 5 || g_state != ST_RECEIVING) return;
      uint32_t off = rd32(d + 1);
      uint32_t n = len - 5;
      // 流量制御は PC 側の「応答ありの書き込み」で行うので、通常は ACK を返さない。
      // 抜けたときだけ NAK でやり直し位置を知らせる。
      if (off < g_received) {        // 受信済みの再送 → 捨てる
        g_statDup++;
        return;
      }
      if (off > g_received) {        // 抜けがあった → ここから送り直してと頼む
        g_statGap++;
        if (!g_nakSent) {
          g_nakSent = true;
          if (!send_ack(0x82, g_received)) g_nakPending = true;
        }
        return;
      }
      if (off + n > g_total) n = g_total - off;
      memcpy(g_buf + off, d + 5, n);
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
      g_crcExpected = rd32(d + 1);
      g_state = ST_VERIFY;
      break;
    }
    case 0x04:  // STOP
      g_state = ST_IDLE;
      g_uiDirty = true;
      break;
  }
}

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
      // 速く転送したいので短い接続間隔 (7.5〜15ms) と 2M PHY を要求する
      sl_bt_connection_set_parameters(c, 6, 12, 0, 400, 0, 0xffff);
      sl_bt_connection_set_preferred_phy(c, 0x02, 0xff);
      Serial.println("connected");
      break;
    }

    case sl_bt_evt_connection_closed_id:
      Serial.println("disconnected");
      g_conn = 0xff;
      g_txNotify = false;
      if (g_state == ST_RECEIVING) g_state = ST_IDLE;
      g_uiDirty = (g_state != ST_PLAYING);
      ble_start_advertising();
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

    case sl_bt_evt_gatt_server_attribute_value_id:
      if (evt->data.evt_gatt_server_attribute_value.attribute == h_rx) {
        on_rx(evt->data.evt_gatt_server_attribute_value.value.data,
              evt->data.evt_gatt_server_attribute_value.value.len);
      }
      break;

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
  uint8_t v[9];
  v[0] = PROTO_VER;
  v[1] = g_mtu & 0xff;
  v[2] = g_mtu >> 8;
  wr32(v + 3, BUF_SIZE);
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
  sc = sl_bt_gattdb_add_uuid128_characteristic(gattdb_session_id, h_service,
                                               SL_BT_GATTDB_CHARACTERISTIC_WRITE | SL_BT_GATTDB_CHARACTERISTIC_WRITE_NO_RESPONSE,
                                               0, 0, make_uuid(0x1001),
                                               sl_bt_gattdb_variable_length_value, 244, 1, zero, &h_rx);
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
