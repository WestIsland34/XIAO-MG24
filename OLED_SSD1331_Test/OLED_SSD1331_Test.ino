/*
 * XIAO MG24 + 0.95" カラーOLED (秋月 QT095B / SSD1331 96x64 RGB) 動作確認
 *
 * ボード   : Silicon Labs → Seeed Studio XIAO MG24 (Sense)
 * Protocol stack : None
 * ライブラリ: Adafruit SSD1331 OLED Driver Library for Arduino, Adafruit GFX Library
 *
 * 配線 (OLED → XIAO MG24)
 *   GND → GND
 *   VCC → 3V3
 *   SCL → D8  (SCK)
 *   SDA → D10 (MOSI)
 *   RES → D3
 *   DC  → D2
 *   CS  → D1
 */
#include <Adafruit_GFX.h>
#include <Adafruit_SSD1331.h>

#define OLED_CS    D1
#define OLED_DC    D2
#define OLED_RST   D3
#define OLED_MOSI  D10   // 基板表記 SDA
#define OLED_SCLK  D8    // 基板表記 SCL

// ソフトウェアSPI (確実に動く)
Adafruit_SSD1331 display(OLED_CS, OLED_DC, OLED_MOSI, OLED_SCLK, OLED_RST);
// ハードウェアSPI (高速。映らなければ上に戻す)
// Adafruit_SSD1331 display(&SPI, OLED_CS, OLED_DC, OLED_RST);

// 16bitカラー (RGB565)
#define BLACK   0x0000
#define RED     0xF800
#define GREEN   0x07E0
#define BLUE    0x001F
#define YELLOW  0xFFE0
#define WHITE   0xFFFF

void setup() {
  Serial.begin(115200);
  delay(1000);
  display.begin();
  Serial.println("SSD1331 init OK");

  display.fillScreen(RED);   delay(500);
  display.fillScreen(GREEN); delay(500);
  display.fillScreen(BLUE);  delay(500);
  display.fillScreen(BLACK);

  display.setTextColor(YELLOW);
  display.setTextSize(1);
  display.setCursor(0, 0);
  display.println("XIAO MG24");
  display.setTextColor(WHITE);
  display.println("Hello OLED!");
}

void loop() {
  static int x = 0;
  display.fillRect(0, 40, 96, 10, BLACK);   // 前の四角を消す
  display.fillRect(x, 40, 10, 10, GREEN);   // 動く四角
  x = (x + 4) % 86;
  delay(50);
}
