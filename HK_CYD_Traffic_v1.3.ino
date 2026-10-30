/*
  ESP32 CYD 2.8" / ESP32-2432S028
  Hong Kong Live Traffic Monitor
  Firmware Version: 1.3.0
  Source filename: HK_CYD_Traffic_v1.3.ino

  功能：
  1. 官方隧道分時段收費 CSV（PC 私家車）
  2. 官方 JTIS 第二代實時行車時間 XML
  3. 官方 Special Traffic News 英文 XML
  4. 官方交通快拍 JPEG
  5. 開機 Cover Page 顯示 3 秒
  6. 觸控設定頁：TUNNEL / NEWS / CCTV 延遲可調 1 至 9 秒
  7. 設定儲存在 ESP32 NVS，重新開機後保留

  顯示會自動輪流切換：
  - TOLL：四條隧道收費 + 行車時間
  - ROADS：主要道路行車時間（每頁 6 項）
  - NEWS：特別交通消息
  - CCTV：交通快拍
  - 觸碰螢幕任何位置：開啟設定頁

  注意：TFT_eSPI 的 User_Setup 必須先按你的 CYD 板子正確設定。
*/

#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <HTTPUpdate.h>
#include <time.h>
#include <TFT_eSPI.h>
#include <WiFiManager.h>
#include <TJpg_Decoder.h>
#include <Preferences.h>
#include "cover_image.h"

TFT_eSPI tft = TFT_eSPI();

// 每次發布新韌體時，把 manifest.txt 的版本號和 .bin 網址更新。
const char* FIRMWARE_VERSION = "1.3.0";
const char* OTA_MANIFEST_URL = "https://raw.githubusercontent.com/Simon-Kan/HK_CYD_Traffic_OTA/main/manifest.txt";
const unsigned long OTA_CHECK_MS = 24UL * 60UL * 60UL * 1000UL;
unsigned long lastOtaCheck = 0;

const long GMT_OFFSET_SEC = 8 * 3600;
const int DAYLIGHT_OFFSET_SEC = 0;
const char* NTP_SERVER = "stdtime.gov.hk";

const char* JTIS_URL = "https://resource.data.one.gov.hk/td/jss/Journeytimev2.xml";
const char* NEWS_URL = "https://resource.data.one.gov.hk/td/en/specialtrafficnews.xml";
const char* TOLL_URL = "https://static.data.gov.hk/td/road-network-v2/TUN_BRIDGE_TV_TOLL.csv";

const char* CCTV_URLS[] = {
  "https://tdcctv.data.one.gov.hk/H207F.JPG",  // Cross Harbour Tunnel HK side
  "https://tdcctv.data.one.gov.hk/K107F.JPG",  // Cross Harbour Tunnel Kowloon side
  "https://tdcctv.data.one.gov.hk/K952F.JPG",  // Eastern Harbour Crossing
  "https://tdcctv.data.one.gov.hk/H702F.JPG",  // Western Harbour Crossing HK side
  "https://tdcctv.data.one.gov.hk/K901F.JPG",  // Western Harbour Crossing Kowloon side
  "https://tdcctv.data.one.gov.hk/TL102F.JPG", // Tai Lam Tunnel Yuen Long side
  "https://tdcctv.data.one.gov.hk/TL103F.JPG", // Tai Lam Tunnel Ting Kau side
  "https://tdcctv.data.one.gov.hk/ST725F.JPG", // Lion Rock Tunnel
  "https://tdcctv.data.one.gov.hk/ST922F.JPG", // Lion Rock Tunnel Hung Mui Kuk
  "https://tdcctv.data.one.gov.hk/SM933F.JPG", // Shing Mun Tunnel
  "https://tdcctv.data.one.gov.hk/SM934F.JPG", // Shing Mun Tunnel Shatin side
  "https://tdcctv.data.one.gov.hk/TH110F.JPG", // Tolo Highway Science Park
  "https://tdcctv.data.one.gov.hk/TH114F.JPG", // Tolo Highway Island House
  "https://tdcctv.data.one.gov.hk/TR101F.JPG", // Tuen Mun Road Chai Wan Kok
  "https://tdcctv.data.one.gov.hk/TR103F.JPG", // Tuen Mun Road Yau Kom Tau
  "https://tdcctv.data.one.gov.hk/TS104F.JPG", // Tsing Sha Highway
  "https://tdcctv.data.one.gov.hk/H210F.JPG",  // Aberdeen Tunnel Wan Chai
  "https://tdcctv.data.one.gov.hk/H421F.JPG"   // Aberdeen Tunnel Aberdeen side
};
const char* CCTV_NAMES[] = {
  "CHT HK", "CHT KLN", "EHC", "WHC HK", "WHC KLN", "TAI LAM YL",
  "TAI LAM TK", "LION ROCK 1", "LION ROCK 2", "SHING MUN 1",
  "SHING MUN 2", "TOLO SCI PARK", "TOLO ISLAND HOUSE", "TUEN MUN RD 1",
  "TUEN MUN RD 2", "TSING SHA", "ABERDEEN WAN CHAI", "ABERDEEN SOUTH"
};
const int CCTV_COUNT = sizeof(CCTV_URLS) / sizeof(CCTV_URLS[0]);

struct JtiItem {
  const char* location;
  const char* destination;
  const char* label;
  int minutes;
  int colour;
  bool valid;
};

JtiItem jti[] = {
  {"H1",  "CH",   "CHT",       -1, -1, false},
  {"H1",  "EH",   "EHC",       -1, -1, false},
  {"H1",  "WH",   "WHC",       -1, -1, false},
  {"SJ5", "TKTL", "TAI LAM",   -1, -1, false},
  {"H1",  "ABT",  "ABERDEEN",  -1, -1, false},
  {"K07", "TSCA", "ROUTE 8",   -1, -1, false},
  {"N06", "TLH",  "TOLO HWY",  -1, -1, false},
  {"SJ2", "TCT",  "TATES CAIRN",-1,-1, false},
  {"N05", "KTPR", "TAI PO RD", -1, -1, false},
  {"N03", "ACTT", "ROUTE 3",   -1, -1, false},
  {"N08", "TKOT", "TSEUNG KWAN O",-1,-1,false},
  {"SJ1", "LRT",  "LION ROCK", -1, -1, false}
};
const int JTI_COUNT = sizeof(jti) / sizeof(jti[0]);

String jtisXml;
String newsXml;
int tollCht = -1, tollEhc = -1, tollWhc = -1, tollTlt = -1;
int page = 0;
int roadPage = 0;
int newsPage = 0;
int cctvIndex = 0;
int displayMode = 0;
unsigned long lastMode = 0;
unsigned long lastJtis = 0;
unsigned long lastNews = 0;
unsigned long lastTolls = 0;
unsigned long lastCctv = 0;
// 使用者可在觸控設定頁修改，並儲存在 ESP32 NVS；有效範圍 1 至 9 秒。
Preferences preferences;
uint8_t tunnelDelaySec = 3;
uint8_t newsDelaySec = 5;
uint8_t cctvDelaySec = 3;
bool settingsOpen = false;
uint16_t touchCalData[5] = {275, 3620, 264, 3532, 1};
const unsigned long JTIS_MS = 120000;
const unsigned long NEWS_MS = 60000;
// 官方分時段收費會按當前香港時間計算；每 2 分鐘重新確認一次。
const unsigned long TOLL_MS = 120000;
const unsigned long CCTV_MS = 120000;

String tagValue(const String& xml, const String& tag, int from = 0) {
  String a = "<" + tag + ">";
  String b = "</" + tag + ">";
  int p = xml.indexOf(a, from);
  if (p < 0) return "";
  p += a.length();
  int q = xml.indexOf(b, p);
  if (q < 0) return "";
  String s = xml.substring(p, q);
  s.trim();
  return s;
}

String xmlItem(const String& xml, const String& location, const String& destination) {
  int p = 0;
  while (true) {
    p = xml.indexOf("<jtis_journey_time>", p);
    if (p < 0) return "";
    int q = xml.indexOf("</jtis_journey_time>", p);
    if (q < 0) return "";
    String block = xml.substring(p, q);
    if (tagValue(block, "LOCATION_ID") == location &&
        tagValue(block, "DESTINATION_ID") == destination) return block;
    p = q + 20;
  }
}

void fetchJtis() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, JTIS_URL)) return;
  http.setTimeout(8000);
  int code = http.GET();
  if (code == HTTP_CODE_OK) jtisXml = http.getString();
  http.end();

  for (int i = 0; i < JTI_COUNT; i++) {
    String b = xmlItem(jtisXml, jti[i].location, jti[i].destination);
    jti[i].valid = b.length() > 0;
    if (jti[i].valid) {
      jti[i].minutes = tagValue(b, "JOURNEY_DATA").toInt();
      jti[i].colour = tagValue(b, "COLOUR_ID").toInt();
      if (tagValue(b, "JOURNEY_TYPE").toInt() != 1) jti[i].minutes = -2;
    }
  }
}

// CSV 欄位：name, chinese, id1, id2, date, day, start, end, toll, class, remarks, updated
int csvField(const String& line, int wanted) {
  int field = 0, start = 0;
  for (int i = 0; i <= line.length(); i++) {
    if (i == line.length() || line[i] == ',') {
      if (field == wanted) return line.substring(start, i).toInt();
      field++; start = i + 1;
    }
  }
  return -1;
}

String csvTextField(const String& line, int wanted) {
  int field = 0, start = 0;
  for (int i = 0; i <= line.length(); i++) {
    if (i == line.length() || line[i] == ',') {
      if (field == wanted) return line.substring(start, i);
      field++; start = i + 1;
    }
  }
  return "";
}

int hhmmss(const String& s) {
  if (s.length() < 8) return -1;
  return s.substring(0,2).toInt() * 3600 + s.substring(3,5).toInt() * 60 + s.substring(6,8).toInt();
}

void fetchTolls() {
  struct tm ti;
  if (!getLocalTime(&ti)) return;
  int nowSec = ti.tm_hour * 3600 + ti.tm_min * 60 + ti.tm_sec;
  // A = Monday-Saturday, B = Sunday/general holiday.
  char dayCode = (ti.tm_wday == 0) ? 'B' : 'A';

  // 四條隧道共用同一份 CSV，只下載一次，避免啟動時下載四次。
  tollCht = tollEhc = tollWhc = tollTlt = -1;
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, TOLL_URL)) return;
  http.setTimeout(15000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) { http.end(); return; }
  WiFiClient* stream = http.getStreamPtr();
  String line = "";
  unsigned long started = millis();
  while (millis() - started < 15000 && (http.connected() || stream->available())) {
    while (stream->available()) {
      char c = stream->read();
      if (c == '\n') {
        if (line.length() > 10 && line.indexOf("TUNNEL_BRIDGE_NAME") < 0) {
          String name = csvTextField(line, 0);
          String day = csvTextField(line, 5);
          String start = csvTextField(line, 6);
          String end = csvTextField(line, 7);
          String vehicle = csvTextField(line, 9);
          int toll = csvField(line, 8);
          if (day.length() && day[0] == dayCode && vehicle == "PC" &&
              nowSec >= hhmmss(start) && nowSec <= hhmmss(end)) {
            if (name == "Cross Harbour Tunnel") tollCht = toll;
            else if (name == "Eastern Harbour Crossing") tollEhc = toll;
            else if (name == "Western Harbour Crossing") tollWhc = toll;
            else if (name == "Tai Lam Tunnel") tollTlt = toll;
          }
        }
        line = "";
      } else if (c != '\r') {
        if (line.length() < 500) line += c;
      }
    }
    delay(1);
  }
  http.end();
}

void fetchNews() {
  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, NEWS_URL)) return;
  http.setTimeout(8000);
  int code = http.GET();
  if (code == HTTP_CODE_OK) newsXml = http.getString();
  http.end();
}

uint16_t trafficColour(int colour) {
  if (colour == 1) return TFT_RED;
  if (colour == 2) return TFT_YELLOW;
  if (colour == 3) return TFT_GREEN;
  return TFT_WHITE;
}

void loadSettings() {
  preferences.begin("traffic", true);
  tunnelDelaySec = constrain(preferences.getUChar("tunnel", 3), 1, 9);
  newsDelaySec = constrain(preferences.getUChar("news", 5), 1, 9);
  cctvDelaySec = constrain(preferences.getUChar("cctv", 3), 1, 9);
  preferences.end();
}

void saveSettings() {
  preferences.begin("traffic", false);
  preferences.putUChar("tunnel", tunnelDelaySec);
  preferences.putUChar("news", newsDelaySec);
  preferences.putUChar("cctv", cctvDelaySec);
  preferences.end();
}

void showSettingsPage() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextFont(4);
  tft.setTextSize(1);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("SETTINGS", 6, 2, 4);

  const int rowY[] = {38, 88, 138};
  const char* labels[] = {"TUNNEL", "NEWS", "CCTV"};
  const uint8_t values[] = {tunnelDelaySec, newsDelaySec, cctvDelaySec};
  for (int i = 0; i < 3; i++) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(labels[i], 8, rowY[i], 4);
    tft.setTextColor(TFT_YELLOW, TFT_BLACK);
    tft.drawString(String(values[i]) + "s", 185, rowY[i], 4);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("<", 270, rowY[i], 4);
    tft.drawString(">", 300, rowY[i], 4);
  }

  tft.fillRoundRect(70, 190, 180, 42, 5, TFT_DARKGREEN);
  tft.setTextColor(TFT_WHITE, TFT_DARKGREEN);
  tft.drawString("SAVE", 118, 198, 4);
  drawVersionFooter();
}

void waitTouchRelease() {
  uint16_t x, y;
  while (tft.getTouch(x, y)) delay(20);
  delay(150);
}

void handleSettingsTouch() {
  uint16_t x, y;
  if (!tft.getTouch(x, y)) return;
  if (y >= 190) {
    saveSettings();
    settingsOpen = false;
    displayMode = 0;
    lastMode = millis();
    waitTouchRelease();
    showToll();
    return;
  }

  int row = (y >= 30 && y < 80) ? 0 :
            (y >= 80 && y < 130) ? 1 :
            (y >= 130 && y < 180) ? 2 : -1;
  if (row >= 0 && (x < 80 || x > 245)) {
    uint8_t* value = (row == 0) ? &tunnelDelaySec :
                     (row == 1) ? &newsDelaySec : &cctvDelaySec;
    if (x < 80 && *value > 1) (*value)--;
    if (x > 245 && *value < 9) (*value)++;
    showSettingsPage();
  }
  waitTouchRelease();
}

void header(const char* title) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(title, 8, 5, 2);
  struct tm ti; char clockText[8] = "--:--";
  if (getLocalTime(&ti)) sprintf(clockText, "%02d:%02d", ti.tm_hour, ti.tm_min);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextDatum(TR_DATUM);
  tft.drawRightString(clockText, 312, 5, 2);
  tft.setTextDatum(TL_DATUM);
  tft.drawFastHLine(5, 25, 310, TFT_DARKGREY);
  drawVersionFooter();
}

void showToll() {
  header("HK TOLL + JOURNEY TIME");
  const char* names[] = {"CHT", "EHC", "WHC", "TLT"};
  int tolls[] = {tollCht, tollEhc, tollWhc, tollTlt};
  int ys[] = {58, 98, 138, 178};
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("TUNNEL", 10, 29, 1);
  tft.drawString("FEE", 110, 29, 1);
  tft.drawString("TIME", 220, 29, 1);
  for (int i = 0; i < 4; i++) {
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(names[i], 10, ys[i], 4);
    tft.setTextColor(tolls[i] < 0 ? TFT_RED : TFT_GREEN, TFT_BLACK);
    tft.drawString(tolls[i] < 0 ? "N/A" : "$" + String(tolls[i]), 110, ys[i], 4);
    tft.setTextColor(jti[i].valid ? trafficColour(jti[i].colour) : TFT_RED, TFT_BLACK);
    // 行車時間使用與收費相同的 4 號字體，並縮短為 5m 以適應 320px 寬度。
    String jt = (jti[i].minutes >= 0) ? String(jti[i].minutes) + "m" : "N/A";
    tft.drawString(jt, 220, ys[i], 4);
  }
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Official TD live time-varying toll", 10, 220, 1);
}

void showRoads() {
  header("LIVE ROAD JOURNEY TIME");
  tft.setTextDatum(TL_DATUM);
  // Font 4 較大，因此每頁顯示 4 條道路，逐頁輪換全部項目。
  int start = roadPage * 4;
  for (int row = 0; row < 4; row++) {
    int i = start + row;
    if (i >= JTI_COUNT) break;
    int y = 34 + row * 40;
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(jti[i].label, 8, y, 4);
    tft.setTextColor(jti[i].valid ? trafficColour(jti[i].colour) : TFT_RED, TFT_BLACK);
    String s = (!jti[i].valid) ? "N/A" :
               (jti[i].minutes >= 0 ? (jti[i].colour == 3 ? "Smooth" : String(jti[i].minutes) + "m") : "CLOSED");
    tft.drawRightString(s, 310, y, 4);
  }
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("Page " + String(roadPage + 1) + "/3  Green Amber Red", 8, 215, 1);
}

bool isDriverNews(const String& text) {
  String s = text;
  s.toLowerCase();
  const char* keywords[] = {
    "road", "tunnel", "highway", "traffic", "accident", "crash",
    "collision", "vehicle", "breakdown", "lane", "carriageway",
    "closed", "closure", "diversion", "congestion", "jam", "fire",
    "flood", "roadwork", "works", "bridge", "flyover", "interchange"
  };
  const int keywordCount = sizeof(keywords) / sizeof(keywords[0]);
  for (int i = 0; i < keywordCount; i++) {
    if (s.indexOf(keywords[i]) >= 0) return true;
  }
  return false;
}

int driverNewsCount() {
  int count = 0;
  int p = 0;
  while (true) {
    p = newsXml.indexOf("<message>", p);
    if (p < 0) break;
    int e = newsXml.indexOf("</message>", p);
    if (e < 0) break;
    String block = newsXml.substring(p, e);
    String text = tagValue(block, "EngShort");
    if (text.length() == 0) text = tagValue(block, "EngText");
    if (isDriverNews(text)) count++;
    p = e + 10;
  }
  return count;
}

String driverNewsAt(int wanted) {
  int count = 0;
  int p = 0;
  while (true) {
    p = newsXml.indexOf("<message>", p);
    if (p < 0) break;
    int e = newsXml.indexOf("</message>", p);
    if (e < 0) break;
    String block = newsXml.substring(p, e);
    String text = tagValue(block, "EngShort");
    if (text.length() == 0) text = tagValue(block, "EngText");
    if (isDriverNews(text)) {
      if (count == wanted) return text;
      count++;
    }
    p = e + 10;
  }
  return "";
}

void showNews() {
  // 使用 TFT_eSPI 內置 Font 2，避免部分 CYD User_Setup 未載入 Font 3 而黑屏。
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  tft.setTextPadding(0);
  tft.setTextFont(2);
  tft.setTextSize(1);
  int total = driverNewsCount();
  String text = driverNewsAt(newsPage);
  tft.setTextColor(TFT_YELLOW, TFT_BLACK);
  String title = "DRIVER NEWS " + String(total > 0 ? newsPage + 1 : 0) + "/" + String(total);
  tft.drawString(title, 4, 4, 2);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.setTextFont(4);
  tft.setTextSize(1);
  if (text.length() == 0) {
    tft.drawString("No message", 4, 35);
    drawVersionFooter();
    return;
  }

  // 按實際像素寬度和完整單字換行，充分使用 320px 寬度，避免出現
  // "Chatham R" / "oad" 這種被固定字符數切斷的情況。
  text.replace("\r", " ");
  text.replace("\n", " ");
  int y = 32;
  String line = "";
  String word = "";
  for (int i = 0; i <= text.length() && y <= 220; i++) {
    char c = (i < text.length()) ? text[i] : ' ';
    if (c != ' ') {
      word += c;
      continue;
    }
    if (word.length() == 0) continue;
    String candidate = line.length() ? line + " " + word : word;
    if (line.length() && tft.textWidth(candidate, 4) > 312) {
      tft.drawString(line.c_str(), 4, y);
      y += 28;
      line = word;
    } else {
      line = candidate;
    }
    word = "";
  }
  if (line.length() && y <= 220) tft.drawString(line.c_str(), 4, y);
  drawVersionFooter();
}

bool jpgCallback(int16_t x, int16_t y, uint16_t w, uint16_t h, uint16_t* bitmap) {
  // CCTV 原圖本身就是 320x240，不能再加上 28 像素標題偏移，
  // 否則圖片下方會被推出螢幕而看不到完整畫面。
  if (y >= tft.height() || x >= tft.width()) return false;
  tft.pushImage(x, y, w, h, bitmap);
  return true;
}

void showCoverPage() {
  tft.fillScreen(TFT_BLACK);
  tft.setTextDatum(TL_DATUM);
  TJpgDec.drawJpg(0, 0, coverJpg, coverJpgLen);
  drawVersionFooter();
  delay(3000);
}

void showCctv() {
  header(CCTV_NAMES[cctvIndex]);
  // 先顯示大字載入提示，讓使用者知道裝置正在下載圖片。
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString("Loading CCTV", 10, 70, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(CCTV_NAMES[cctvIndex], 10, 115, 4);
  drawVersionFooter();
  delay(1000);

  WiFiClientSecure client; client.setInsecure();
  HTTPClient http;
  if (!http.begin(client, CCTV_URLS[cctvIndex])) return;
  http.setFollowRedirects(HTTPC_STRICT_FOLLOW_REDIRECTS);
  http.setTimeout(12000);
  http.addHeader("User-Agent", "Mozilla/5.0");
  http.addHeader("Accept", "image/jpeg,image/*,*/*");
  int code = http.GET();
  if (code == HTTP_CODE_OK) {
    int len = http.getSize();
    // 部分官方鏡頭圖片偶爾會大於 64 KB，避免下載完整但被截斷。
    const int MAX_JPG_SIZE = 100000;
    int capacity = (len > 0 && len < MAX_JPG_SIZE) ? len : MAX_JPG_SIZE;
    uint8_t* buf = (uint8_t*)malloc(capacity);
    if (buf) {
      WiFiClient* s = http.getStreamPtr();
      int got = 0;
      unsigned long lastData = millis();
      while ((http.connected() || s->available()) && got < capacity && millis() - lastData < 5000) {
        int availableBytes = s->available();
        if (availableBytes > 0) {
          int want = min(availableBytes, capacity - got);
          int n = s->readBytes((char*)buf + got, want);
          if (n > 0) {
            got += n;
            lastData = millis();
          }
        } else {
          delay(2);
        }
      }
      JRESULT result = (got > 1000) ? TJpgDec.drawJpg(0, 0, buf, got) : JDR_INP;
      free(buf);
      if (result != JDR_OK) {
        tft.fillScreen(TFT_BLACK);
        tft.setTextColor(TFT_RED, TFT_BLACK);
        tft.drawString("JPG decode error", 10, 90, 4);
        tft.drawString("HTTP " + String(code) + " / " + String(got), 10, 125, 2);
      }
    } else {
      tft.fillScreen(TFT_BLACK);
      tft.setTextColor(TFT_RED, TFT_BLACK);
      tft.drawString("Not enough memory", 10, 90, 2);
    }
  } else {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("CCTV HTTP error", 10, 90, 2);
    tft.drawString(String(code), 10, 120, 2);
  }
  http.end();
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(CCTV_NAMES[cctvIndex], 8, 218, 1);
  drawVersionFooter();
}

void drawVersionFooter() {
  tft.setTextDatum(BR_DATUM);
  tft.setTextColor(TFT_LIGHTGREY, TFT_BLACK);
  tft.drawString("v" + String(FIRMWARE_VERSION), 315, 238, 1);
  tft.setTextDatum(TL_DATUM);
}

void loadingScreen(const char* title, const String& detail) {
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_CYAN, TFT_BLACK);
  tft.drawString(title, 10, 60, 4);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString(detail, 10, 110, 2);
  drawVersionFooter();
}

bool newerVersion(const String& remote, const char* current) {
  int r1, r2, r3, c1, c2, c3;
  if (sscanf(remote.c_str(), "%d.%d.%d", &r1, &r2, &r3) != 3) return false;
  if (sscanf(current, "%d.%d.%d", &c1, &c2, &c3) != 3) return false;
  if (r1 != c1) return r1 > c1;
  if (r2 != c2) return r2 > c2;
  return r3 > c3;
}

void checkForRemoteUpdate() {
  if (WiFi.status() != WL_CONNECTED) return;
  if (String(OTA_MANIFEST_URL).indexOf("YOUR_GITHUB_USERNAME") >= 0) return;

  WiFiClientSecure manifestClient;
  manifestClient.setInsecure();
  HTTPClient http;
  if (!http.begin(manifestClient, OTA_MANIFEST_URL)) return;
  http.setTimeout(8000);
  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    http.end();
    return;
  }

  String manifest = http.getString();
  http.end();
  int firstEnd = manifest.indexOf('\n');
  if (firstEnd < 0) return;
  String remoteVersion = manifest.substring(0, firstEnd);
  remoteVersion.trim();
  String firmwareUrl = manifest.substring(firstEnd + 1);
  firmwareUrl.trim();
  if (!newerVersion(remoteVersion, FIRMWARE_VERSION) || firmwareUrl.length() < 20) return;

  loadingScreen("Updating firmware", "New version " + remoteVersion);
  delay(1000);

  httpUpdate.onStart([]() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_CYAN, TFT_BLACK);
    tft.drawString("Updating firmware", 10, 55, 4);
    drawVersionFooter();
  });
  httpUpdate.onProgress([](int current, int total) {
    int percent = total > 0 ? (current * 100 / total) : 0;
    tft.fillRect(10, 110, 300, 20, TFT_DARKGREY);
    tft.fillRect(10, 110, percent * 3, 20, TFT_GREEN);
    tft.setTextColor(TFT_WHITE, TFT_BLACK);
    tft.drawString(String(percent) + "%", 125, 150, 4);
    drawVersionFooter();
  });
  httpUpdate.onEnd([]() {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_GREEN, TFT_BLACK);
    tft.drawString("Update complete", 10, 90, 4);
    drawVersionFooter();
  });
  httpUpdate.onError([](int error) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("Update failed", 10, 80, 4);
    tft.drawString(String(error), 10, 125, 2);
    drawVersionFooter();
    delay(1500);
  });

  WiFiClientSecure firmwareClient;
  firmwareClient.setInsecure();
  t_httpUpdate_return result = httpUpdate.update(firmwareClient, firmwareUrl);
  if (result != HTTP_UPDATE_OK) delay(1500);
}

void setup() {
  Serial.begin(115200);
  tft.init();
  tft.setRotation(1);
  tft.setTouch(touchCalData);
  loadSettings();
  tft.fillScreen(TFT_BLACK);
  TJpgDec.setCallback(jpgCallback);
  TJpgDec.setJpgScale(1);
  TJpgDec.setSwapBytes(true);
  showCoverPage();
  tft.fillScreen(TFT_BLACK);
  tft.setTextColor(TFT_WHITE, TFT_BLACK);
  tft.drawString("Hong Kong Live", 5, 45, 2);
  tft.drawString("Traffic Monitor", 5, 75, 2);
  tft.drawString("AP: HK-Toll-Setup", 10, 110, 2);
  drawVersionFooter();

  // 與已驗證可連線的參考程式相同，明確使用 STA 模式。
  // 關閉省電可避免部分 CYD/路由器在配網後經常斷線。
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFiManager wm;
  wm.setConfigPortalTimeout(180);
  wm.setConnectTimeout(30);

  // 使用與參考程式完全相同的 AP 名稱。
  // 若之前保存的 Wi-Fi 已失效，WiFiManager 會建立此 AP 供重新設定。
  if (!wm.autoConnect("HK-Toll-Setup")) {
    tft.fillScreen(TFT_BLACK);
    tft.setTextColor(TFT_RED, TFT_BLACK);
    tft.drawString("WiFi Timeout", 10, 80, 4);
    tft.drawString("Restarting...", 10, 125, 2);
    delay(3000);
    ESP.restart();
  }

  loadingScreen("WiFi Connected", WiFi.localIP().toString());
  delay(1000);

  loadingScreen("Checking update", "Firmware v" + String(FIRMWARE_VERSION));
  checkForRemoteUpdate();
  lastOtaCheck = millis();

  configTime(GMT_OFFSET_SEC, DAYLIGHT_OFFSET_SEC, NTP_SERVER);

  loadingScreen("Loading data", "Journey time / traffic status");
  fetchJtis();
  loadingScreen("Loading data", "Special traffic news");
  fetchNews();
  loadingScreen("Loading data", "Tunnel toll information");
  fetchTolls();
  loadingScreen("Ready", "Starting HK traffic monitor");
  delay(1200);
  lastMode = millis();
}

void loop() {
  unsigned long now = millis();
  if (settingsOpen) {
    handleSettingsTouch();
    delay(20);
    return;
  }
  uint16_t touchX, touchY;
  if (tft.getTouch(touchX, touchY)) {
    settingsOpen = true;
    showSettingsPage();
    waitTouchRelease();
    return;
  }
  if (now - lastOtaCheck >= OTA_CHECK_MS) {
    loadingScreen("Checking update", "Firmware v" + String(FIRMWARE_VERSION));
    checkForRemoteUpdate();
    lastOtaCheck = millis();
  }
  if (now - lastJtis > JTIS_MS || jtisXml.length() == 0) { fetchJtis(); lastJtis = now; }
  if (now - lastNews > NEWS_MS || newsXml.length() == 0) { fetchNews(); lastNews = now; }
  if (now - lastTolls > TOLL_MS || tollCht < 0) { fetchTolls(); lastTolls = now; }

  unsigned long screenDelay = (displayMode == 2) ?
                              (unsigned long)newsDelaySec * 1000UL :
                              (displayMode == 3 ? (unsigned long)cctvDelaySec * 1000UL :
                              (unsigned long)tunnelDelaySec * 1000UL);
  if (now - lastMode >= screenDelay) {
    if (displayMode == 0) {
      showToll();
      displayMode = 1;
    } else if (displayMode == 1) {
      showRoads();
      roadPage = (roadPage + 1) % 3;
      newsPage = 0;
      displayMode = 2;
    } else if (displayMode == 2) {
      showNews();
      int totalDriver = driverNewsCount();
      if (totalDriver > 0) {
        newsPage++;
        if (newsPage >= totalDriver) {
          newsPage = 0;
          displayMode = 3;
        }
      } else {
        newsPage = 0;
        displayMode = 3;
      }
    } else {
      // CCTV 模式會連續顯示完整鏡頭清單，全部完成後才回到收費頁。
      showCctv();
      cctvIndex++;
      if (cctvIndex >= CCTV_COUNT) {
        cctvIndex = 0;
        displayMode = 0;
      }
    }
    lastMode = now;
  }
  delay(50);
}
