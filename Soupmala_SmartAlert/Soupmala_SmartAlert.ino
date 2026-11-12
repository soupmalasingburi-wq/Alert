// SOUPMALA SMART ALERT - Phase 1 (ESP32-2432S028 / TPM408-2.8)
// Library: LovyanGFX (lovyan03). Core: esp32:esp32
// มือถือส่งข้อความแจ้งเตือนมาที่ http://<IP บอร์ด>/n?key=...&text=...

#include <WiFi.h>
#include <WebServer.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <time.h>
#define LGFX_USE_V1
#include <LovyanGFX.hpp>

// ===== ตั้งค่า =====
// Wi-Fi และรหัสลับตั้งผ่านหน้าเว็บ (ต่อ Wi-Fi ชื่อ SoupMala-Setup) ไม่ต้องแก้โค้ด
String secretKey = "soupmala123";
#define USE_ST7789 1                      // บอร์ด 2 USB มักเป็น ST7789; ถ้าจอเพี้ยนให้ลองเปลี่ยนเป็น 0 (ILI9341)
const uint32_t ALERT_MS = 8000;           // แสดงแจ้งเตือนใหญ่กี่ ms
const uint32_t DEDUP_MS = 10000;          // กันแจ้งซ้ำข้อความเดิม
// ===================

#if USE_ST7789
using PanelT = lgfx::Panel_ST7789;
const bool PANEL_INVERT = true;
#else
using PanelT = lgfx::Panel_ILI9341;
const bool PANEL_INVERT = false;
#endif

class LGFX : public lgfx::LGFX_Device {
  PanelT _panel;
  lgfx::Bus_SPI _bus;
public:
  LGFX() {
    {
      auto c = _bus.config();
      c.spi_host = SPI2_HOST;
      c.spi_mode = 0;
      c.freq_write = 40000000;
      c.freq_read = 16000000;
      c.use_lock = true;
      c.dma_channel = SPI_DMA_CH_AUTO;
      c.pin_sclk = 14;
      c.pin_mosi = 13;
      c.pin_miso = 12;
      c.pin_dc = 2;
      _bus.config(c);
      _panel.setBus(&_bus);
    }
    {
      auto c = _panel.config();
      c.pin_cs = 15;
      c.pin_rst = -1;
      c.pin_busy = -1;
      c.panel_width = 240;
      c.panel_height = 320;
      c.offset_x = 0;
      c.offset_y = 0;
      c.readable = false;
      c.invert = PANEL_INVERT;
      c.rgb_order = false;
      c.bus_shared = false;
      _panel.config(c);
    }
    setPanel(&_panel);
  }
};

LGFX tft;
WebServer server(80);

enum Mode { IDLE, PAID, ORDER };
Mode mode = IDLE;
uint32_t alertStart = 0;
float lastAmount = 0;
float totalToday = 0;
int paidCount = 0, orderCount = 0;
int lastDay = -1;
String lastText;
uint32_t lastTextMs = 0;
bool needDraw = true;

float parseAmount(const String& s) {
  int i = s.indexOf("ได้รับชำระเงิน");
  if (i < 0) return -1;
  i += String("ได้รับชำระเงิน").length();
  String num;
  bool started = false;
  for (; i < (int)s.length(); i++) {
    char c = s[i];
    if (c >= '0' && c <= '9') { num += c; started = true; }
    else if (started && c == ',') continue;
    else if (started && c == '.') num += c;
    else if (started) break;
  }
  if (num.length() == 0) return -1;
  return num.toFloat();
}

void checkNewDay() {
  struct tm t;
  if (!getLocalTime(&t, 0)) return;
  if (lastDay != -1 && t.tm_mday != lastDay) {
    totalToday = 0; paidCount = 0; orderCount = 0; needDraw = true;
  }
  lastDay = t.tm_mday;
}

void draw() {
  tft.setRotation(1);  // แนวนอน 320x240
  tft.setTextDatum(lgfx::middle_center);
  if (mode == PAID) {
    tft.fillScreen(TFT_DARKGREEN);
    tft.setTextColor(TFT_WHITE);
    tft.setFont(&fonts::Font4);
    tft.setTextSize(1);
    tft.drawString("RECEIVED", 160, 50);
    tft.setTextSize(3);
    char b[24]; snprintf(b, sizeof(b), "%.2f", lastAmount);
    tft.drawString(b, 160, 120);
    tft.setTextSize(1);
    tft.drawString("THB", 160, 185);
  } else if (mode == ORDER) {
    tft.fillScreen(TFT_ORANGE);
    tft.setTextColor(TFT_BLACK);
    tft.setFont(&fonts::Font4);
    tft.setTextSize(2);
    tft.drawString("NEW ORDER", 160, 90);
    tft.setTextSize(1);
    tft.drawString("Wongnai", 160, 160);
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_WHITE);
    tft.setFont(&fonts::Font2);
    tft.setTextSize(1);
    tft.drawString("SOUPMALA SMART ALERT", 160, 20);
    tft.setFont(&fonts::Font4);
    tft.setTextColor(TFT_GREEN);
    tft.drawString("Today", 160, 65);
    tft.setTextSize(2);
    char b[24]; snprintf(b, sizeof(b), "%.0f", totalToday);
    tft.drawString(b, 160, 115);
    tft.setTextSize(1);
    tft.setTextColor(TFT_WHITE);
    char c[40]; snprintf(c, sizeof(c), "Paid %d   Orders %d", paidCount, orderCount);
    tft.drawString(c, 160, 170);
    tft.setFont(&fonts::Font2);
    tft.setTextColor(TFT_DARKGREY);
    String ip = WiFi.status() == WL_CONNECTED ? WiFi.localIP().toString() : "No WiFi";
    tft.drawString(ip, 160, 220);
  }
  needDraw = false;
}

void handleNotify() {
  if (server.arg("key") != secretKey) { server.send(403, "text/plain", "forbidden"); return; }
  String text = server.arg("text");
  if (text.length() == 0) { server.send(400, "text/plain", "no text"); return; }
  if (text == lastText && millis() - lastTextMs < DEDUP_MS) { server.send(200, "text/plain", "dup"); return; }
  lastText = text; lastTextMs = millis();

  float amt = parseAmount(text);
  if (amt >= 0) {
    lastAmount = amt; totalToday += amt; paidCount++;
    mode = PAID; alertStart = millis(); needDraw = true;
    server.send(200, "text/plain", "paid");
  } else if (text.indexOf("ออเดอร์") >= 0) {
    orderCount++;
    mode = ORDER; alertStart = millis(); needDraw = true;
    server.send(200, "text/plain", "order");
  } else {
    server.send(200, "text/plain", "ignored");
  }
}

void setup() {
  Serial.begin(115200);
  pinMode(21, OUTPUT); digitalWrite(21, HIGH);  // backlight
  tft.init();
  tft.setRotation(1);
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE);
  tft.setFont(&fonts::Font4);
  tft.setTextDatum(lgfx::middle_center);
  tft.drawString("Connecting WiFi...", 160, 120);

  pinMode(0, INPUT_PULLUP);
  WiFiManager wm;
  if (digitalRead(0) == LOW) wm.resetSettings();   // กดค้างปุ่ม BOOT ตอนเปิดเครื่อง = ล้าง Wi-Fi
  Preferences pref;
  pref.begin("sm", false);
  secretKey = pref.getString("key", secretKey);
  WiFiManagerParameter pKey("key", "Secret key", secretKey.c_str(), 20);
  wm.addParameter(&pKey);
  tft.fillScreen(TFT_BLACK);
  tft.setFont(&fonts::Font2);
  tft.drawString("Connect phone to WiFi:", 160, 90);
  tft.setFont(&fonts::Font4);
  tft.drawString("SoupMala-Setup", 160, 130);
  wm.setConfigPortalTimeout(300);
  wm.autoConnect("SoupMala-Setup");
  secretKey = pKey.getValue();
  pref.putString("key", secretKey);
  pref.end();
  configTime(7 * 3600, 0, "pool.ntp.org", "time.google.com");  // เวลาไทย

  server.on("/n", handleNotify);
  server.on("/", []() { server.send(200, "text/plain", "SoupMala Smart Alert OK"); });
  server.begin();
  needDraw = true;
}

void loop() {
  server.handleClient();
  if (WiFi.status() != WL_CONNECTED) {
    static uint32_t lastTry = 0;
    if (millis() - lastTry > 15000) { lastTry = millis(); WiFi.reconnect(); }
  }
  if (mode != IDLE && millis() - alertStart > ALERT_MS) { mode = IDLE; needDraw = true; }
  static uint32_t lastChk = 0;
  if (millis() - lastChk > 30000) { lastChk = millis(); checkNewDay(); }
  if (needDraw) draw();
}
