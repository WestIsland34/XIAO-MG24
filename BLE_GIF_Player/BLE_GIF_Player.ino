/*
 * XIAO MG24 BLE GIF Player
 *
 * PC のブラウザ (docs/index.html, Web Bluetooth) から送られてきた
 * GIF アニメ (96x64 / 256色に変換済み) を受信して XIAO 基板上の外付けフラッシュ
 * (SPI1, CS=PA6) に保存し、SSD1331 OLED でループ再生する。
 * 送ったアニメはフラッシュにキャッシュとして残り (古いものから上書き)、PC から
 * 番号を指定するだけで再生できる。電源を入れ直すと、最後に再生したアニメを自動で再生する。
 * 加速度センサー (XIAO MG24 Sense 内蔵 LSM6DS3TR-C) が 1 分間動きを感じないと
 * 画面を消してスリープし、動かすと再開する。
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
 *          0x01 BEGIN  u32 totalLen, u32 crc32, u8 nameLen, name (UTF-8, 最大 43 バイト)
 *                      同じ CRC・サイズのアニメがキャッシュにあれば、受信せずにそれを
 *                      再生して DONE (cached=1) を返す。
 *          0x02 DATA   u32 offset, payload...
 *          0x03 END    u32 crc32 (IEEE, 全データ)
 *          0x04 STOP
 *          0x05 LIST   キャッシュの一覧を ENTRY ... LIST_END で返す
 *          0x06 PLAY   u32 id   キャッシュのアニメを再生する
 *          ---- ライブ表示 (PC の画面をそのまま映す。フラッシュには書かない) ----
 *          0x07 LIVE_START
 *          0x08 LIVE_PAL   u8 start, u16 color (RGB565) ...   パレットの一部を設定
 *          0x09 LIVE_DATA  u8 seq, u8 flags (bit0=キーフレーム), u16 offset, u16 total, data...
 *                          1 コマ分の差分データ (アニメの frame data と同じ形式) を分割して送る。
 *                          キーフレームは「全画素 0」からの差分。届いたらすぐ描く。
 *          0x0A LIVE_END   ライブ表示をやめて、最後に再生していたアニメに戻る
 *          PC は DATA を 12 個に 1 個「応答あり」で書き込み、その応答を待ってから
 *          次を送る。XIAO はキューが詰まっているときだけ処理後に応答する (= 流量制御)。
 *   TX   ...1002  Notify (XIAO → PC)
 *          0x81 ACK    u32 receivedBytes    (BEGIN の受付時)
 *          0x82 NAK    u32 expectedOffset   (抜けを検出。この位置から送り直して)
 *          0x83 DONE   u8 result (0=OK 1=CRC 2=FORMAT 3=TOO_LARGE 4=NO_FLASH), u8 cached
 *          0x84 ENTRY  u32 id, u32 size, u32 crc, u8 playing, u8 nameLen, name
 *          0x85 LIST_END u16 count
 *          0x86 PLAYED u8 result (0=OK 1=見つからない 2=FORMAT)
 *          0x87 NEED_KEY   ライブ表示のデータが抜けたので、次はキーフレームを送って
 *   INFO ...1003  Read: u8 protoVer, u16 mtu, u32 maxBytes, u8 width, u8 height,
 *                       u8 fwMajor, u8 fwMinor, u8 fwPatch
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
 *   0x000000, 0x001000 : 目録 (4KB ずつ 2 か所に交互に書く。seq が大きく CRC が正しい方が有効)
 *              ヘッダ 64 バイト + 項目 64 バイト x 63 (id, addr, size, crc, name)
 *   0x002000 : 「いま再生中の id」の記録 (4 バイトずつ追記。一杯になったら消去)
 *   0x003000 〜: アニメデータ本体。4KB 単位で前から順に置き、端まで来たら先頭に戻って
 *              古いものを上書きする (リングバッファ)。受信しながら 4KB ずつ消去して書く。
 */
#include <SPI.h>
#include <Wire.h>
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

#define FW_VER_MAJOR 1
#define FW_VER_MINOR 2
#define FW_VER_PATCH 0
#define PROTO_VER  4
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
#define DIR_ADDR_A      0x000000u
#define DIR_ADDR_B      0x001000u
#define CUR_ADDR        0x002000u   // 再生中 id の追記ログ
#define FLASH_DATA_ADDR 0x003000u
#define FLASH_SECTOR    4096u
#define FLASH_PAGE      256u
#define FRAME_MAX       8192u       // 1 フレームの差分データの上限 (実際は最大 6.3KB 程度)

static SPISettings g_flashSpi(FLASH_HZ, MSBFIRST, SPI_MODE0);
static uint32_t g_flashSize = 0;    // 0 = 見つからない
static uint8_t  g_flashId[3];
static uint8_t  g_fbuf[FRAME_MAX];  // フレーム読み出し・CRC 計算用

// ---------------- キャッシュの目録 ----------------
#define DIR_MAX   63
#define NAME_MAX  43
struct DirEntry {       // 64 バイト。id == 0 は空き
  uint32_t id, addr, size, crc, reserved;
  uint8_t  nameLen;
  char     name[NAME_MAX];
};
struct DirHdr {         // 64 バイト
  char     magic[4];    // "GAD1"
  uint32_t seq, writePtr, nextId, crc;
  uint8_t  pad[44];
};
struct Dir {
  DirHdr   h;
  DirEntry e[DIR_MAX];
};
static_assert(sizeof(DirEntry) == 64 && sizeof(Dir) == 4096, "dir layout");
static Dir      g_dir;
static uint32_t g_curId = 0;        // いま再生中 (最後に再生した) アニメの id
static uint16_t g_curPos = 0;       // 再生中 id ログの次の書き込み位置 (4 バイト単位)
static int16_t  g_listPos = -1;     // LIST の送信位置 (-1 = 送っていない)
static uint16_t g_listCount = 0;

// ---------------- 状態 ----------------
// 状態の変更とフラッシュの読み書きは loop() だけが行う。
// BLE のイベント処理は受信パケットをキューに積むだけ。
enum : uint8_t { ST_IDLE, ST_RECEIVING, ST_PLAYING, ST_ERROR, ST_LIVE };

// ライブ表示: 1 コマ分のデータを g_fbuf に集めてから描く
static uint8_t  g_liveSeq = 0;
static uint32_t g_liveLen = 0;
static bool     g_liveBroken = false;   // このコマは途中が抜けた
static bool     g_liveNeedKey = true;   // キーフレームが来るまで描かない
static bool     g_needKeyPending = false;
static uint32_t g_liveFrames = 0, g_liveBytes = 0, g_liveDrawUs = 0, g_liveDropped = 0;
static uint8_t  g_state = ST_IDLE;
static uint32_t g_total = 0;
static uint32_t g_received = 0;
static uint32_t g_erasedUpTo = 0;   // ここ (フラッシュのアドレス) までは消去済み
static uint32_t g_recvAddr = 0;     // 受信中のアニメを置くフラッシュのアドレス
static uint32_t g_recvCrc = 0;
static uint8_t  g_recvNameLen = 0;
static char     g_recvName[NAME_MAX];
static bool     g_nakSent = false;
static bool     g_ackPending = false, g_nakPending = false;
static int16_t  g_donePending = -1;
static uint8_t  g_doneCached = 0;
static int16_t  g_playedPending = -1;
static uint32_t g_statDup = 0, g_statGap = 0, g_statNotifyFail = 0;
static bool     g_uiDirty = true;
static uint8_t  g_errorCode = 0;

static volatile uint8_t  g_conn = 0xff;      // 接続ハンドル (0xff = 未接続)
static volatile uint16_t g_mtu = 23;
static volatile bool     g_bleActivity = false;  // BLE で何か操作があった (スリープ解除用)

// ---------------- 加速度センサーとスリープ ----------------
#define IMU_PWR       19        // PD5: IMU の電源
#define IMU_ADDR      0x6A
#define SLEEP_MS      60000u    // この時間動きがなければスリープ
#define MOTION_TH     820       // 動いたとみなす変化 (1LSB = 0.061mg, 820 ≒ 0.05g)
#define IMU_POLL_MS   50
static bool     g_imuOk = false;
static int16_t  g_accRef[3];         // 最後に「動いた」と判定したときの値
static uint32_t g_lastActive = 0;    // 最後に動いた (または操作があった) 時刻
static bool     g_sleeping = false;

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
  uint8_t m[3] = { 0x83, result, g_doneCached };
  return notify(m, 3);
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
  if (g_needKeyPending) {
    uint8_t m[1] = { 0x87 };
    if (notify(m, 1)) g_needKeyPending = false;
  }
  if (g_playedPending >= 0) {
    uint8_t m[2] = { 0x86, (uint8_t)g_playedPending };
    if (notify(m, 2)) g_playedPending = -1;
  }
  // キャッシュの一覧を 1 項目ずつ送る (送れなければ次の loop でやり直す)
  while (g_listPos >= 0) {
    if (g_listPos >= DIR_MAX) {
      uint8_t m[3] = { 0x85, (uint8_t)g_listCount, (uint8_t)(g_listCount >> 8) };
      if (!notify(m, 3)) break;
      g_listPos = -1;
      break;
    }
    const DirEntry &e = g_dir.e[g_listPos];
    if (e.id) {
      uint8_t m[15 + NAME_MAX];
      m[0] = 0x84;
      wr32(m + 1, e.id); wr32(m + 5, e.size); wr32(m + 9, e.crc);
      m[13] = (e.id == g_curId && g_state == ST_PLAYING) ? 1 : 0;
      m[14] = e.nameLen;
      memcpy(m + 15, e.name, e.nameLen);
      if (!notify(m, 15 + e.nameLen)) break;
      g_listCount++;
    }
    g_listPos++;
  }
}

// ---------------- 画面 ----------------
static void screen_message(const char *l1, const char *l2, uint16_t color) {
  display.fillScreen(0x0000);
  display.setTextSize(1);
  display.setTextColor(color);
  display.setCursor(2, 4);
  display.print("GIF Player v"); display.print(FW_VER_MAJOR); display.print("."); display.print(FW_VER_MINOR);
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
static uint8_t parse_anim(uint32_t base, uint32_t total) {
  uint8_t h[12];
  if (total < 12) return 2;
  fl_read(base, h, 12);
  if (memcmp(h, "GA01", 4) != 0 || h[4] != SCR_W || h[5] != SCR_H) return 2;
  uint16_t frames = rd16(h + 6);
  uint16_t ncol = rd16(h + 8);
  if (frames == 0 || ncol == 0 || ncol > 256) return 2;
  uint32_t pos = 12 + (uint32_t)ncol * 2;
  if (pos > total) return 2;
  uint8_t pb[512];
  fl_read(base + 12, pb, ncol * 2);
  memset(g_pal, 0, sizeof(g_pal));
  for (uint16_t i = 0; i < ncol; i++) g_pal[i] = rd16(pb + i * 2);
  uint32_t start = pos;
  for (uint16_t f = 0; f < frames; f++) {
    if (pos + 4 > total) return 2;
    uint8_t fh[4];
    fl_read(base + pos, fh, 4);
    uint16_t len = rd16(fh + 2);
    if (len > FRAME_MAX) return 2;
    pos += 4 + len;
  }
  if (pos != total) return 2;
  g_total = total;
  g_frames = frames;
  g_framesStart = base + start;
  g_framePos = g_framesStart;
  g_frameNo = 0;
  g_nextAt = millis();
  g_forceFull = true;
  return 0;
}

// ---------------- キャッシュの目録 ----------------
static uint32_t dir_crc(Dir &d) {
  uint32_t save = d.h.crc;
  d.h.crc = 0;
  uint32_t c = crc32_update(0xFFFFFFFFu, (const uint8_t *)&d, sizeof(Dir)) ^ 0xFFFFFFFFu;
  d.h.crc = save;
  return c;
}
static bool dir_valid(Dir &d) {
  if (memcmp(d.h.magic, "GAD1", 4) != 0 || d.h.crc != dir_crc(d)) return false;
  for (int i = 0; i < DIR_MAX; i++) {
    DirEntry &e = d.e[i];
    if (e.id && (e.addr < FLASH_DATA_ADDR || e.addr + e.size > g_flashSize || e.nameLen > NAME_MAX)) e.id = 0;
  }
  return true;
}
static void dir_save() {
  g_dir.h.seq++;
  g_dir.h.crc = dir_crc(g_dir);
  uint32_t a = (g_dir.h.seq & 1) ? DIR_ADDR_B : DIR_ADDR_A;   // 古い方に書く
  fl_erase_sector(a);
  fl_program(a, (const uint8_t *)&g_dir, sizeof(Dir));
}
static void dir_load() {
  static_assert(sizeof(g_fbuf) >= sizeof(Dir), "fbuf");
  Dir *tmp = (Dir *)g_fbuf;
  fl_read(DIR_ADDR_A, (uint8_t *)&g_dir, sizeof(Dir));
  bool okA = dir_valid(g_dir);
  fl_read(DIR_ADDR_B, (uint8_t *)tmp, sizeof(Dir));
  bool okB = dir_valid(*tmp);
  if (okB && (!okA || tmp->h.seq > g_dir.h.seq)) {
    memcpy(&g_dir, tmp, sizeof(Dir));
  } else if (!okA) {
    memset(&g_dir, 0, sizeof(Dir));
    memcpy(g_dir.h.magic, "GAD1", 4);
    g_dir.h.writePtr = FLASH_DATA_ADDR;
    g_dir.h.nextId = 1;
    Serial.println("cache: new directory");
  }
  // 再生中 id のログ: 最後に書かれた値を探す。その後ろが消去済みでなければ (古い形式の
  // データが残っているなど) 追記できないので、消去してやり直す
  fl_read(CUR_ADDR, g_fbuf, FLASH_SECTOR);
  g_curPos = 0;
  g_curId = 0;
  for (uint16_t i = 0; i < FLASH_SECTOR / 4; i++) {
    uint32_t v = rd32(g_fbuf + i * 4);
    if (v == 0xFFFFFFFFu) break;
    g_curId = v;
    g_curPos = i + 1;
  }
  bool clean = true;
  for (uint32_t i = g_curPos * 4; i < FLASH_SECTOR; i++) if (g_fbuf[i] != 0xFF) { clean = false; break; }
  if (!clean || (!okA && !okB)) {
    fl_erase_sector(CUR_ADDR);
    g_curPos = 0;
    g_curId = 0;
  }
}
// 再生中の id を記録する。毎回目録を書き直すと消去回数が増えるので、別の領域に追記する
static void cur_save(uint32_t id) {
  if (id == g_curId) return;
  if (g_curPos >= FLASH_SECTOR / 4) { fl_erase_sector(CUR_ADDR); g_curPos = 0; }
  uint8_t b[4];
  wr32(b, id);
  fl_program(CUR_ADDR + g_curPos * 4, b, 4);
  g_curPos++;
  g_curId = id;
}
static int dir_find(uint32_t id) {
  if (id == 0) return -1;
  for (int i = 0; i < DIR_MAX; i++) if (g_dir.e[i].id == id) return i;
  return -1;
}
static int dir_find_crc(uint32_t crc, uint32_t size) {
  for (int i = 0; i < DIR_MAX; i++)
    if (g_dir.e[i].id && g_dir.e[i].crc == crc && g_dir.e[i].size == size) return i;
  return -1;
}
static uint32_t dir_count() {
  uint32_t n = 0;
  for (int i = 0; i < DIR_MAX; i++) if (g_dir.e[i].id) n++;
  return n;
}
// 新しいアニメの置き場所を決め、重なる古いアニメを目録から消す
static uint32_t dir_alloc(uint32_t total) {
  uint32_t need = (total + FLASH_SECTOR - 1) / FLASH_SECTOR * FLASH_SECTOR;
  uint32_t a = g_dir.h.writePtr;
  if (a < FLASH_DATA_ADDR || a + need > g_flashSize) a = FLASH_DATA_ADDR;   // 端まで来たら先頭へ
  bool changed = false;
  for (int i = 0; i < DIR_MAX; i++) {
    DirEntry &e = g_dir.e[i];
    if (e.id && e.addr < a + need && a < e.addr + e.size) {
      Serial.printf("cache: drop id %lu (%.*s)\n", (unsigned long)e.id, e.nameLen, e.name);
      e.id = 0;
      changed = true;
    }
  }
  if (dir_count() >= DIR_MAX) {           // 項目が一杯 → いちばん古いものを消す
    int oldest = -1;
    for (int i = 0; i < DIR_MAX; i++)
      if (g_dir.e[i].id && (oldest < 0 || g_dir.e[i].id < g_dir.e[oldest].id)) oldest = i;
    g_dir.e[oldest].id = 0;
    changed = true;
  }
  if (changed) dir_save();
  return a;
}

// 目録の項目を再生する
static uint8_t play_entry(int i) {
  if (i < 0) return 1;
  const DirEntry &e = g_dir.e[i];
  uint8_t r = parse_anim(e.addr, e.size);
  if (r) return r;
  cur_save(e.id);
  g_state = ST_PLAYING;
  g_uiDirty = true;
  Serial.printf("play: id %lu %.*s (%lu bytes, %u frames)\n", (unsigned long)e.id, e.nameLen, e.name,
                (unsigned long)e.size, g_frames);
  return 0;
}

// 起動時: 最後に再生していたアニメがあれば再生する
static bool load_saved_anim() {
  dir_load();
  Serial.printf("cache: %lu anims, seq %lu, writePtr %06lX, current id %lu\n", (unsigned long)dir_count(),
                (unsigned long)g_dir.h.seq, (unsigned long)g_dir.h.writePtr, (unsigned long)g_curId);
  int i = dir_find(g_curId);
  if (i < 0) return false;
  DirEntry &e = g_dir.e[i];
  screen_message("Loading...", "", 0x07FF);
  uint32_t t0 = millis();
  if (flash_crc(e.addr, e.size) != e.crc) {
    Serial.println("saved anim: CRC NG");
    e.id = 0;
    dir_save();
    return false;
  }
  if (play_entry(i) != 0) { Serial.println("saved anim: format NG"); return false; }
  Serial.printf("saved anim checked in %lu ms\n", (unsigned long)(millis() - t0));
  return true;
}

// ---------------- 加速度センサー (LSM6DS3TR-C, Wire1, 電源 PD5) ----------------
static bool imu_write(uint8_t reg, uint8_t v) {
  Wire1.beginTransmission(IMU_ADDR);
  Wire1.write(reg);
  Wire1.write(v);
  return Wire1.endTransmission() == 0;
}
static bool imu_read(uint8_t reg, uint8_t *buf, uint8_t n) {
  Wire1.beginTransmission(IMU_ADDR);
  Wire1.write(reg);
  if (Wire1.endTransmission(false) != 0) return false;
  if (Wire1.requestFrom((uint8_t)IMU_ADDR, n) != n) return false;
  for (uint8_t i = 0; i < n; i++) buf[i] = Wire1.read();
  return true;
}
static bool imu_accel(int16_t a[3]) {
  uint8_t b[6];
  if (!imu_read(0x28, b, 6)) return false;     // OUTX_L_XL 〜 OUTZ_H_XL
  for (int i = 0; i < 3; i++) a[i] = (int16_t)(b[i * 2] | (b[i * 2 + 1] << 8));
  return true;
}
static bool imu_init() {
  pinMode(IMU_PWR, OUTPUT);
  digitalWrite(IMU_PWR, HIGH);
  delay(20);
  Wire1.begin();
  Wire1.setClock(400000);
  uint8_t who = 0;
  if (!imu_read(0x0F, &who, 1) || (who != 0x6A && who != 0x69)) {
    Serial.printf("imu: not found (WHO_AM_I %02X) - sleep disabled\n", who);
    return false;
  }
  imu_write(0x12, 0x44);    // CTRL3_C: BDU=1, IF_INC=1
  imu_write(0x10, 0x30);    // CTRL1_XL: 52Hz, ±2g
  delay(50);
  imu_accel(g_accRef);
  Serial.printf("imu: LSM6DS3 (WHO_AM_I %02X) OK\n", who);
  return true;
}
// 最後に動いたときの値から MOTION_TH 以上変わったら「動いた」(ゆっくり傾けても検出できる)
static bool imu_moved() {
  int16_t a[3];
  if (!imu_accel(a)) return false;
  for (int i = 0; i < 3; i++) {
    if (abs((int32_t)a[i] - g_accRef[i]) > MOTION_TH) {
      memcpy(g_accRef, a, sizeof(a));
      return true;
    }
  }
  return false;
}
static void enter_sleep() {
  g_sleeping = true;
  display.enableDisplay(false);     // 画面を消す (画面の中身は OLED 側に残る)
  Serial.println("sleep");
}
static void leave_sleep() {
  g_sleeping = false;
  display.enableDisplay(true);
  g_nextAt = millis();
  g_uiDirty = true;
  Serial.println("wake");
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

// ---------------- ライブ表示 ----------------
static void live_packet(const uint8_t *d, uint8_t len) {
  uint8_t seq = d[1], flags = d[2];
  uint32_t off = rd16(d + 3), total = rd16(d + 5);
  uint32_t n = len - 7;
  if (off == 0) {                       // 新しいコマ
    g_liveSeq = seq;
    g_liveLen = 0;
    g_liveBroken = false;
  }
  if (seq != g_liveSeq || off != g_liveLen || total > sizeof(g_fbuf) || off + n > total) {
    if (!g_liveBroken) {                // 途中が抜けた → このコマは捨ててキーフレームを頼む
      g_liveBroken = true;
      g_liveDropped++;
      g_liveNeedKey = true;
      g_needKeyPending = true;
    }
    return;
  }
  if (g_liveBroken) return;
  memcpy(g_fbuf + off, d + 7, n);
  g_liveLen += n;
  if (g_liveLen < total) return;

  // 1 コマそろった
  g_liveLen = 0;
  bool key = flags & 1;
  if (g_liveNeedKey && !key) return;    // キーフレーム待ち
  g_liveNeedKey = false;
  uint32_t t0 = micros();
  int32_t pmin = NPIX, pmax = -1;
  if (key) memset(g_idx, 0, sizeof(g_idx));
  decode_frame(g_fbuf, total, pmin, pmax);
  if (key || g_forceFull) { pmin = 0; pmax = NPIX - 1; }
  if (pmax >= pmin) push_dirty_tiles(pmin / SCR_W, pmax / SCR_W, g_forceFull);
  g_forceFull = false;
  g_liveDrawUs += micros() - t0;
  g_liveFrames++;
  g_liveBytes += total;
}
// ライブ表示をやめて、最後に再生していたアニメに戻る
static void live_end() {
  Serial.println("live: end");
  if (play_entry(dir_find(g_curId)) != 0) {
    g_state = ST_IDLE;
    g_uiDirty = true;
  }
}

static void handle_packet(const uint8_t *d, uint8_t len) {
  if (len == 0) return;
  switch (d[0]) {
    case 0x01: {  // BEGIN
      g_doneCached = 0;
      if (len < 10) { g_donePending = 2; return; }
      uint32_t total = rd32(d + 1);
      uint32_t crc = rd32(d + 5);
      uint8_t nameLen = d[9];
      if (nameLen > NAME_MAX) nameLen = NAME_MAX;
      if (10 + nameLen > len) nameLen = len - 10;
      if (!g_flashSize) { g_errorCode = 4; set_state(ST_ERROR); g_donePending = 4; return; }
      if (total == 0 || total > flash_max_bytes()) {
        g_errorCode = 3; set_state(ST_ERROR); g_donePending = 3; return;
      }
      int hit = dir_find_crc(crc, total);
      if (hit >= 0 && play_entry(hit) == 0) {    // キャッシュにある → 送ってもらわずに再生
        g_doneCached = 1;
        g_donePending = 0;
        return;
      }
      g_recvCrc = crc;
      g_recvNameLen = nameLen;
      memcpy(g_recvName, d + 10, nameLen);
      g_recvAddr = dir_alloc(total);
      g_total = total;
      g_received = 0;
      g_erasedUpTo = g_recvAddr;
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
      uint32_t a = g_recvAddr + off;
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
      uint8_t r = (crc == g_recvCrc && flash_crc(g_recvAddr, g_total) == crc) ? 0 : 1;
      if (r) {
        uint8_t h[16];
        fl_read(g_recvAddr, h, 16);
        Serial.printf("CRC NG. head: %02X %02X %02X %02X %02X %02X %02X %02X\n",
                      h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
      }
      if (r == 0) r = parse_anim(g_recvAddr, g_total);
      Serial.printf("verify: %u (%lu bytes, %u frames, %lu ms)\n", r, (unsigned long)g_total,
                    g_frames, (unsigned long)(millis() - t0));
      if (r == 0) {
        // 目録に登録する。これで電源を切っても残る (dir_alloc で空きは確保済み)
        int slot = 0;
        for (int i = 0; i < DIR_MAX; i++) if (!g_dir.e[i].id) { slot = i; break; }
        DirEntry &e = g_dir.e[slot];
        memset(&e, 0, sizeof(e));
        e.id = g_dir.h.nextId++;
        e.addr = g_recvAddr;
        e.size = g_total;
        e.crc = crc;
        e.nameLen = g_recvNameLen;
        memcpy(e.name, g_recvName, g_recvNameLen);
        g_dir.h.writePtr = g_recvAddr + (g_total + FLASH_SECTOR - 1) / FLASH_SECTOR * FLASH_SECTOR;
        dir_save();
        cur_save(e.id);
        Serial.printf("cache: saved id %lu at %06lX (%lu anims)\n", (unsigned long)e.id,
                      (unsigned long)e.addr, (unsigned long)dir_count());
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
    case 0x05:  // LIST
      g_listPos = 0;
      g_listCount = 0;
      break;
    case 0x06: {  // PLAY
      if (len < 5 || g_state == ST_RECEIVING) { g_playedPending = 1; return; }
      uint8_t r = play_entry(dir_find(rd32(d + 1)));
      if (r) Serial.printf("play: id %lu failed (%u)\n", (unsigned long)rd32(d + 1), r);
      g_playedPending = r;
      break;
    }
    case 0x07:  // LIVE_START
      if (g_state == ST_RECEIVING) return;
      g_state = ST_LIVE;
      g_liveLen = 0;
      g_liveBroken = false;
      g_liveNeedKey = true;
      g_liveFrames = g_liveBytes = g_liveDrawUs = g_liveDropped = 0;
      memset(g_pal, 0, sizeof(g_pal));
      Serial.println("live: start");
      break;
    case 0x08: {  // LIVE_PAL
      if (len < 2 || g_state != ST_LIVE) return;
      uint16_t start = d[1];
      for (uint16_t i = 0; 2 + i * 2 + 1 < len && start + i < 256; i++) g_pal[start + i] = rd16(d + 2 + i * 2);
      g_forceFull = true;   // 色が変わったので次は全画面を描く
      break;
    }
    case 0x09:  // LIVE_DATA
      if (len >= 7 && g_state == ST_LIVE) live_packet(d, len);
      break;
    case 0x0A:  // LIVE_END
      if (g_state == ST_LIVE) live_end();
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
  Serial.printf("BLE GIF Player v%d.%d.%d\n", FW_VER_MAJOR, FW_VER_MINOR, FW_VER_PATCH);
  g_imuOk = imu_init();
  g_lastActive = millis();

  if (fl_init()) {
    char msg[64];
    bool ok = fl_selftest(msg, sizeof(msg));
    Serial.printf("flash: id %02X %02X %02X, %lu KB, selftest %s: %s\n", g_flashId[0], g_flashId[1],
                  g_flashId[2], (unsigned long)(g_flashSize / 1024), ok ? "OK" : "NG", msg);
    char l1[24];
    snprintf(l1, sizeof(l1), "Flash %luKB %s", (unsigned long)(g_flashSize / 1024), ok ? "OK" : "NG");
    screen_message(l1, ok ? "" : "RW test failed", ok ? 0x07E0 : 0xF800);
    delay(ok ? 800 : 5000);
    load_saved_anim();
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

  // ---- スリープ: 1 分間動きも操作もなければ画面を消す。動かすか BLE で操作すると戻る ----
  {
    static uint32_t lastPoll = 0;
    uint32_t now = millis();
    bool active = false;
    if (g_bleActivity) { g_bleActivity = false; active = true; }
    if (g_state == ST_RECEIVING || g_state == ST_LIVE) active = true;
    if (g_imuOk && now - lastPoll >= (g_sleeping ? 100u : IMU_POLL_MS)) {
      lastPoll = now;
      if (imu_moved()) active = true;
    }
    if (active) {
      g_lastActive = now;
      if (g_sleeping) leave_sleep();
    } else if (g_imuOk && !g_sleeping && now - g_lastActive >= SLEEP_MS) {
      enter_sleep();
    }
    if (g_sleeping) {
      if (g_rxTail == g_rxHead) delay(20);
      return;
    }
  }

  // 受信中に切断されたら待ち受けに戻る
  if (g_state == ST_RECEIVING && g_conn == 0xff) set_state(ST_IDLE);
  // ライブ表示中に切断されたらアニメに戻る
  if (g_state == ST_LIVE && g_conn == 0xff) live_end();
  if (g_state == ST_LIVE) {
    static uint32_t lastStat = 0;
    uint32_t now = millis();
    if (now - lastStat >= 2000) {
      if (g_liveFrames)
        Serial.printf("live: %lu.%lu fps, %lu B/frame, draw %lu us/frame, dropped %lu\n",
                      (unsigned long)(g_liveFrames * 10000 / (now - lastStat) / 10),
                      (unsigned long)(g_liveFrames * 10000 / (now - lastStat) % 10),
                      (unsigned long)(g_liveBytes / g_liveFrames), (unsigned long)(g_liveDrawUs / g_liveFrames),
                      (unsigned long)g_liveDropped);
      g_liveFrames = g_liveBytes = g_liveDrawUs = 0;
      lastStat = now;
    }
  }

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
      g_bleActivity = true;
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
      g_bleActivity = true;
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
  uint8_t v[12];
  v[0] = PROTO_VER;
  v[1] = g_mtu & 0xff;
  v[2] = g_mtu >> 8;
  wr32(v + 3, flash_max_bytes());
  v[7] = SCR_W;
  v[8] = SCR_H;
  v[9] = FW_VER_MAJOR;
  v[10] = FW_VER_MINOR;
  v[11] = FW_VER_PATCH;
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

  uint8_t zero[12] = { 0 };
  // RX: アプリ管理の値 (書き込みは user_write_request イベントで届き、応答はアプリが返す)
  sc = sl_bt_gattdb_add_uuid128_characteristic(gattdb_session_id, h_service,
                                               SL_BT_GATTDB_CHARACTERISTIC_WRITE | SL_BT_GATTDB_CHARACTERISTIC_WRITE_NO_RESPONSE,
                                               0, 0, make_uuid(0x1001),
                                               sl_bt_gattdb_user_managed_value, 244, 0, zero, &h_rx);
  app_assert_status(sc);
  sc = sl_bt_gattdb_add_uuid128_characteristic(gattdb_session_id, h_service,
                                               SL_BT_GATTDB_CHARACTERISTIC_NOTIFY,
                                               0, 0, make_uuid(0x1002),
                                               sl_bt_gattdb_variable_length_value, 244, 1, zero, &h_tx);
  app_assert_status(sc);
  sc = sl_bt_gattdb_add_uuid128_characteristic(gattdb_session_id, h_service,
                                               SL_BT_GATTDB_CHARACTERISTIC_READ,
                                               0, 0, make_uuid(0x1003),
                                               sl_bt_gattdb_fixed_length_value, 12, 12, zero, &h_info);
  app_assert_status(sc);
  sc = sl_bt_gattdb_start_service(gattdb_session_id, h_service);
  app_assert_status(sc);

  sc = sl_bt_gattdb_commit(gattdb_session_id);
  app_assert_status(sc);
  ble_update_info();
}
