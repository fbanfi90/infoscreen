#include <Arduino.h>
#include <WiFi.h>
#include <Wire.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Arduino_GFX_Library.h>
#include <math.h>
#include <time.h>

// ---------- config ----------
// Personal settings come from .env (see .env.example) via load_env.py.
// The defaults below only fill in optional keys.
#ifndef VIA_FILTER
#define VIA_FILTER ""
#endif
#ifndef BOARD_TITLE
#define BOARD_TITLE "Departures"
#endif
#ifndef DEPARTURES_FETCH
#define DEPARTURES_FETCH 10
#endif
#ifndef WEATHER_NAME
#define WEATHER_NAME ""
#endif
#ifndef TZ_POSIX
#define TZ_POSIX "CET-1CEST,M3.5.0,M10.5.0/3"
#endif
#ifndef SCREEN_ROTATION
#define SCREEN_ROTATION 0
#endif

static const uint32_t FETCH_EVERY_MS = 30000;          // departures
static const uint32_t WEATHER_EVERY_MS = 15 * 60000;   // MeteoSwiss updates hourly
static const uint32_t REDRAW_EVERY_MS = 2000;

// ---------- pins (Waveshare wiki, ESP32-S3-Touch-LCD-2.8) ----------
#define PIN_LCD_MOSI 45
#define PIN_LCD_SCLK 40
#define PIN_LCD_CS   42
#define PIN_LCD_DC   41
#define PIN_LCD_RST  39
#define PIN_LCD_BL    5
// touch controller (CST328, I2C)
#define PIN_TP_SDA    1
#define PIN_TP_SCL    3
#define PIN_TP_RST    2

// ---------- screen layout (portrait 240x320) ----------
static const int W = 240;
static const int TRAM_ROW_Y = 22, TRAM_ROW_H = 44;  // 3 rows: 22..150
static const int DIVIDER_Y = 156;
static const int WX_TOP = 160;                       // weather: 160..304
static const int STATUS_Y = 307;                     // status line: 307..318

// ---------- colors (RGB565) ----------
static const uint16_t C_BG     = 0x0000;
static const uint16_t C_AMBER  = 0xFDA0;
static const uint16_t C_ORANGE = 0xFB00;
static const uint16_t C_GREY   = 0x8410;
static const uint16_t C_DARK   = 0x4208;  // ~#404040, for the low-priority status line
static const uint16_t C_LIGHT  = 0xC618;
static const uint16_t C_WHITE  = 0xFFFF;
static const uint16_t C_RED    = 0xF800;
static const uint16_t C_BLUE   = 0x655F;  // ~#64AAFF, rain
static const uint16_t C_YELLOW = 0xFF00;  // sun, lightning

Arduino_DataBus *bus = new Arduino_ESP32SPI(PIN_LCD_DC, PIN_LCD_CS, PIN_LCD_SCLK, PIN_LCD_MOSI, GFX_NOT_DEFINED);
// rotation: 0 = portrait, 2 = portrait flipped 180°; IPS panel -> inverted colors = true
Arduino_GFX *gfx = new Arduino_ST7789(bus, PIN_LCD_RST, SCREEN_ROTATION, true, 240, 320);

// ---------- data ----------
struct Departure {
  char line[8];
  char to[48];  // Latin-1
  time_t when;   // real-time departure (scheduled + delay)
  int delay;     // minutes
};

static Departure deps[6];
static int depCount = 0;
static bool fetchOk = false;
static time_t lastFetchAt = 0;

// MeteoSwiss forecast. All series start at `start` (local midnight today),
// except precip1h, which starts at `lowRes` (midnight tomorrow) where the
// 10-minute precipitation series ends.
struct Weather {
  bool valid = false;
  time_t fetchedAt = 0;
  float curTemp = NAN;
  int curIcon = 0;
  float dayMin = NAN, dayMax = NAN, dayPrecip = NAN;
  time_t start = 0, lowRes = 0;
  float temp1h[48]; int nTemp = 0;     // 48 h is plenty for a 12 h window
  float precip10m[160]; int nP10 = 0;
  float precip1h[48]; int nP1 = 0;
  int icon3h[16]; int nIcon = 0;
  float prob3h[16]; int nProb = 0;
};
static Weather wx;
static bool wxFetchOk = false;
static bool wxDirty = true;  // weather area needs a full redraw

// ---------- text ----------
// Five text styles. Each is either the BDF font load_env.py picked from the
// FONT folder in .env (converted to font_<role>.h in the build directory and
// announced with FONT_<ROLE>_CUSTOM), or by default the graphics library's
// built-in 6x8 font, scaled up.
// f == nullptr means the built-in font. w x h is the character cell in pixels.
struct Font { const GFXfont *f; int w, h; uint8_t scale; };
#define BUILTIN_FONT(s) {nullptr, 6 * (s), 8 * (s), (s)}

#ifdef FONT_SMALL_CUSTOM   // status line, forecast labels
#include "font_small.h"
static const Font F_SMALL = {&font_small, FONT_SMALL_W, FONT_SMALL_H, 1};
#else
static const Font F_SMALL = BUILTIN_FONT(1);
#endif

#ifdef FONT_TEXT_CUSTOM    // destinations, min/max
#include "font_text.h"
static const Font F_TEXT = {&font_text, FONT_TEXT_W, FONT_TEXT_H, 1};
#else
static const Font F_TEXT = BUILTIN_FONT(1);
#endif

#ifdef FONT_HEADER_CUSTOM  // title, clock, place name
#include "font_header.h"
static const Font F_HEADER = {&font_header, FONT_HEADER_W, FONT_HEADER_H, 1};
#else
static const Font F_HEADER = BUILTIN_FONT(2);
#endif

#ifdef FONT_MEDIUM_CUSTOM  // line, departure time, minutes
#include "font_medium.h"
static const Font F_MEDIUM = {&font_medium, FONT_MEDIUM_W, FONT_MEDIUM_H, 1};
#else
static const Font F_MEDIUM = BUILTIN_FONT(2);
#endif

#ifdef FONT_BIG_CUSTOM     // current temperature
#include "font_big.h"
static const Font F_BIG = {&font_big, FONT_BIG_W, FONT_BIG_H, 1};
#else
static const Font F_BIG = BUILTIN_FONT(3);
#endif

// Coordinates passed to text() are the TOP-left of the text; each glyph is a
// full cell w x h pixels. Widths below are computed from w, so other
// monospace fonts still line up; the vertical layout assumes cells no taller
// than the Terminus sizes listed in .env.example.

// Strings in this program are Latin-1; the built-in font is code page 437,
// which has the common accented letters at other positions.
static char latin1ToCp437(uint8_t c) {
  static const uint8_t from[] = {0xB0, 0xB1, 0xB2, 0xB5, 0xB7, 0xA3, 0xC4, 0xC5, 0xC6, 0xC7,
                                 0xC9, 0xD1, 0xD6, 0xDC, 0xDF, 0xE0, 0xE1, 0xE2, 0xE4, 0xE5,
                                 0xE6, 0xE7, 0xE8, 0xE9, 0xEA, 0xEB, 0xEC, 0xED, 0xEE, 0xEF,
                                 0xF1, 0xF2, 0xF3, 0xF4, 0xF6, 0xF9, 0xFA, 0xFB, 0xFC, 0xFF};
  static const uint8_t to[] = {0xF8, 0xF1, 0xFD, 0xE6, 0xFA, 0x9C, 0x8E, 0x8F, 0x92, 0x80,
                               0x90, 0xA5, 0x99, 0x9A, 0xE1, 0x85, 0xA0, 0x83, 0x84, 0x86,
                               0x91, 0x87, 0x8A, 0x82, 0x88, 0x89, 0x8D, 0xA1, 0x8C, 0x8B,
                               0xA4, 0x95, 0xA2, 0x93, 0x94, 0x97, 0xA3, 0x96, 0x81, 0x98};
  if (c < 0x80) return (char)c;
  for (size_t i = 0; i < sizeof from; i++)
    if (from[i] == c) return (char)to[i];
  return '?';
}

// Opaque text (drawn with a background color) overwrites the old glyphs, so
// padding strings to a fixed width gives flicker-free updates.
static void text(int x, int top, const Font &f, uint16_t fg, const char *s, bool opaque = true) {
  gfx->setFont(f.f);
  gfx->setTextSize(f.scale);
  if (opaque) gfx->setTextColor(fg, C_BG);
  else gfx->setTextColor(fg);
  if (f.f) {
    gfx->setCursor(x, top + f.h * 2 / 3);  // GFX fonts: cursor y = Arduino_GFX's baseline
    gfx->print(s);
    return;
  }
  gfx->setCursor(x, top);  // built-in font: cursor = top-left
  char buf[64];
  size_t i = 0;
  for (; s[i] && i + 1 < sizeof buf; i++) buf[i] = latin1ToCp437((uint8_t)s[i]);
  buf[i] = 0;
  gfx->print(buf);
}

// Like text(), but padded with spaces / cut to exactly fill `px` pixels, so
// shorter new text fully covers longer old text.
static void textFit(int x, int top, const Font &f, uint16_t fg, const char *s, int px) {
  char buf[64];
  int n = min((int)sizeof buf - 1, px / f.w);
  snprintf(buf, sizeof buf, "%-*.*s", n, n, s);
  text(x, top, f, fg, buf);
}

// The API sends UTF-8 ("ü" = 0xC3 0xBC); the fonts are Latin-1 ("ü" = 0xFC).
// Two-byte UTF-8 sequences starting with 0xC2/0xC3 map directly to Latin-1.
static void utf8ToLatin1(const char *in, char *out, size_t outSize) {
  size_t o = 0;
  while (*in && o + 1 < outSize) {
    uint8_t c = (uint8_t)*in++;
    if (c < 0x80) {
      out[o++] = (char)c;
    } else if ((c == 0xC2 || c == 0xC3) && *in) {
      out[o++] = (char)(((c & 0x03) << 6) | ((uint8_t)*in++ & 0x3F));
    } else {  // anything else: skip the sequence, show '?'
      while ((((uint8_t)*in) & 0xC0) == 0x80) in++;
      out[o++] = '?';
    }
  }
  out[o] = 0;
}

static void status(const char *msg, uint16_t color) {
  textFit(4, STATUS_Y, F_SMALL, color, msg, W - 8);
}

// ---------- HTTP ----------
// GETs `url` into `body`. Returns false (and logs) on any error.
static bool httpGet(const char *url, String &body) {
  WiFiClientSecure client;
  client.setInsecure();  // public data; skip cert pinning

  HTTPClient http;
  http.setConnectTimeout(10000);
  http.setTimeout(15000);
  if (!http.begin(client, url)) return false;

  int code = http.GET();
  if (code != HTTP_CODE_OK) {
    Serial.printf("HTTP %d for %s\n", code, url);
    http.end();
    return false;
  }
  // Download the whole response first (lands in PSRAM), then parse it.
  body = http.getString();
  http.end();
  Serial.printf("got %u bytes from %s\n", body.length(), url);
  return true;
}

// ---------- departures ----------
static bool fetchDepartures() {
  char url[128];
  snprintf(url, sizeof url, "https://transport.opendata.ch/v1/stationboard?id=%s&limit=%d",
           STATION_ID, DEPARTURES_FETCH);
  String body;
  if (!httpGet(url, body)) return false;

  // Only keep the fields we need in the parsed result.
  JsonDocument filter;
  filter["stationboard"][0]["category"] = true;
  filter["stationboard"][0]["number"] = true;
  filter["stationboard"][0]["to"] = true;
  filter["stationboard"][0]["stop"]["departureTimestamp"] = true;
  filter["stationboard"][0]["stop"]["delay"] = true;
  if (VIA_FILTER[0]) filter["stationboard"][0]["passList"][0]["station"]["name"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(
      doc, body,
      DeserializationOption::Filter(filter),
      DeserializationOption::NestingLimit(20));
  if (err) {
    Serial.printf("departures JSON: %s\n", err.c_str());
    return false;
  }

  int n = 0;
  for (JsonObject j : doc["stationboard"].as<JsonArray>()) {
    if (n >= (int)(sizeof deps / sizeof deps[0])) break;

    if (VIA_FILTER[0]) {
      bool via = false;
      for (JsonObject p : j["passList"].as<JsonArray>()) {
        const char *name = p["station"]["name"] | "";
        if (strstr(name, VIA_FILTER)) { via = true; break; }
      }
      if (!via) continue;
    }

    const char *cat = j["category"] | "";
    const char *num = j["number"] | "";
    Departure &d = deps[n++];
    if (strncmp(num, cat, strlen(cat)) == 0) snprintf(d.line, sizeof d.line, "%s", num);
    else snprintf(d.line, sizeof d.line, "%s%s", cat, num);
    utf8ToLatin1(j["to"] | "", d.to, sizeof d.to);
    d.delay = j["stop"]["delay"] | 0;
    d.when = (time_t)(j["stop"]["departureTimestamp"].as<long>()) + d.delay * 60;
  }
  depCount = n;
  return true;
}

static void drawDepartures(time_t now) {
  struct tm tm;
  char buf[64];

  // header: title + clock
  char title[32];
  utf8ToLatin1(BOARD_TITLE, title, sizeof title);
  int clockX = W - 4 - 5 * F_HEADER.w;
  textFit(4, 2, F_HEADER, C_LIGHT, title, clockX - 8 - 4);
  localtime_r(&now, &tm);
  strftime(buf, sizeof buf, "%H:%M", &tm);
  text(clockX, 2, F_HEADER, C_AMBER, buf);

  // rows: skip anything that already left, show up to 3
  int row = 0;
  for (int i = 0; i < depCount && row < 3; i++) {
    const Departure &d = deps[i];
    if (d.when < now - 30) continue;
    long mins = (long)(d.when - now) / 60;
    int y = TRAM_ROW_Y + row * TRAM_ROW_H;

    textFit(4, y, F_MEDIUM, C_AMBER, d.line, 4 * F_MEDIUM.w);

    localtime_r(&d.when, &tm);
    strftime(buf, sizeof buf, "%H:%M", &tm);
    int timeX = (W - 5 * F_MEDIUM.w) / 2 - 14;  // 76 with the default font
    text(timeX, y, F_MEDIUM, d.delay > 0 ? C_ORANGE : C_AMBER, buf);

    if (mins <= 0) snprintf(buf, sizeof buf, " now");
    else if (mins < 60) snprintf(buf, sizeof buf, "%3ld'", mins);
    else if (mins < 600) snprintf(buf, sizeof buf, "%ldh%02ld", mins / 60, mins % 60);  // 1h23
    else snprintf(buf, sizeof buf, "%3ldh", mins / 60);
    text(W - 4 - 4 * F_MEDIUM.w, y, F_MEDIUM, C_AMBER, buf);

    textFit(4, y + F_MEDIUM.h + 1, F_TEXT, C_LIGHT, d.to, W - 8);
    row++;
  }
  // blank unused rows
  for (; row < 3; row++)
    gfx->fillRect(0, TRAM_ROW_Y + row * TRAM_ROW_H, W, 40, C_BG);
}

// ---------- weather ----------
// Copies a JSON number array into `out`, returns how many were copied.
template <typename T>
static int copyArray(JsonVariantConst arr, T *out, int max, int skip = 0) {
  int n = 0, i = 0;
  for (JsonVariantConst v : arr.as<JsonArrayConst>()) {
    if (i++ < skip) continue;
    if (n >= max) break;
    out[n++] = v.as<T>();
  }
  return n;
}

// Unofficial but stable JSON endpoint behind the MeteoSwiss app: the same
// local forecast as meteoswiss.ch, for a postcode, in ~14 KB.
static bool fetchWeather(time_t now) {
  char url[96];
  // MeteoSwiss wants 6 digits: postcode + 2-digit locality, "00" for the main one.
  snprintf(url, sizeof url, "https://app-prod-ws.meteoswiss-app.ch/v1/plzDetail?plz=%s%s",
           WEATHER_PLZ, strlen(WEATHER_PLZ) == 4 ? "00" : "");
  String body;
  if (!httpGet(url, body)) return false;

  JsonDocument filter;
  filter["currentWeather"]["temperature"] = true;
  filter["currentWeather"]["icon"] = true;
  JsonObject ff = filter["forecast"].add<JsonObject>();
  ff["temperatureMin"] = true;
  ff["temperatureMax"] = true;
  ff["precipitation"] = true;
  JsonObject fg = filter["graph"].to<JsonObject>();
  for (const char *k : {"start", "startLowResolution", "temperatureMean1h", "precipitation10m",
                        "precipitation1h", "weatherIcon3h", "precipitationProbability3h"})
    fg[k] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, body, DeserializationOption::Filter(filter));
  if (err) {
    Serial.printf("weather JSON: %s\n", err.c_str());
    return false;
  }
  JsonObjectConst g = doc["graph"];
  if (g["start"].isNull() || g["temperatureMean1h"].isNull()) {
    Serial.println("weather JSON: missing graph data");
    return false;
  }

  Weather w;
  w.curTemp = doc["currentWeather"]["temperature"] | NAN;
  w.curIcon = doc["currentWeather"]["icon"] | 0;
  JsonObjectConst today = doc["forecast"][0];
  w.dayMin = today["temperatureMin"] | NAN;
  w.dayMax = today["temperatureMax"] | NAN;
  w.dayPrecip = today["precipitation"] | NAN;

  // Keep only a window from the start of the current day-hour onward, so the
  // fixed-size arrays hold the next ~2 days.
  time_t start = (time_t)(g["start"].as<long long>() / 1000);
  time_t lowRes = (time_t)(g["startLowResolution"].as<long long>() / 1000);
  int skipH = now > start ? (int)((now - start) / 3600) : 0;
  skipH -= skipH % 3;  // stay aligned with the 3-hour series
  w.start = start + skipH * 3600;
  w.nTemp = copyArray(g["temperatureMean1h"], w.temp1h, 48, skipH);
  w.nIcon = copyArray(g["weatherIcon3h"], w.icon3h, 16, skipH / 3);
  w.nProb = copyArray(g["precipitationProbability3h"], w.prob3h, 16, skipH / 3);
  w.nP10 = copyArray(g["precipitation10m"], w.precip10m, 160, skipH * 6);
  int skipLow = now > lowRes ? (int)((now - lowRes) / 3600) : 0;
  w.lowRes = lowRes + skipLow * 3600;
  w.nP1 = copyArray(g["precipitation1h"], w.precip1h, 48, skipLow);

  w.valid = true;
  w.fetchedAt = now;
  wx = w;
  return true;
}

// Rain in mm for the hour starting at t, or -1 if unknown.
static float precipHour(time_t t) {
  if (t < wx.lowRes) {
    int i = (int)((t - wx.start) / 600);
    if (i < 0 || i + 6 > wx.nP10) return -1;
    float s = 0;
    for (int k = 0; k < 6; k++) s += wx.precip10m[i + k];
    return s;
  }
  int i = (int)((t - wx.lowRes) / 3600);
  return (i >= 0 && i < wx.nP1) ? wx.precip1h[i] : -1;
}

static float tempHour(time_t t) {
  int i = (int)((t - wx.start) / 3600);
  return (i >= 0 && i < wx.nTemp) ? wx.temp1h[i] : NAN;
}

static int icon3h(time_t t) {
  int i = (int)((t - wx.start) / 10800);
  return (i >= 0 && i < wx.nIcon) ? wx.icon3h[i] : 0;
}

static float prob3h(time_t t) {
  int i = (int)((t - wx.start) / 10800);
  return (i >= 0 && i < wx.nProb) ? wx.prob3h[i] : NAN;
}

// MeteoSwiss weather icon codes: 1..42 by day, +100 at night.
// Each code is reduced to the parts of a small drawn icon.
enum : uint8_t {
  SUN = 1,      // clear sky: big sun/moon alone
  SUN_BEHIND = 2,  // small sun/moon behind a cloud
  CLOUD = 4, DARK = 8,       // light / dark cloud
  RAIN = 16, SNOW = 32,      // both = sleet
  THUNDER = 64, FOG = 128,
};
static uint8_t iconParts(int code) {
  static const uint8_t T[43] = {
    CLOUD,                                  //  0 unknown
    SUN,                                    //  1 sunny
    SUN_BEHIND | CLOUD,                     //  2 mostly sunny
    SUN_BEHIND | CLOUD,                     //  3 partly sunny
    CLOUD | DARK,                           //  4 overcast
    CLOUD | DARK,                           //  5 very cloudy
    SUN_BEHIND | CLOUD | RAIN,              //  6 sunny intervals, isolated showers
    SUN_BEHIND | CLOUD | RAIN | SNOW,       //  7 ... isolated sleet
    SUN_BEHIND | CLOUD | SNOW,              //  8 ... snow showers
    CLOUD | DARK | RAIN,                    //  9 overcast, some showers
    CLOUD | DARK | RAIN | SNOW,             // 10 overcast, some sleet
    CLOUD | DARK | SNOW,                    // 11 overcast, some snow
    SUN_BEHIND | CLOUD | THUNDER,           // 12 sunny intervals, thunderstorm chance
    SUN_BEHIND | CLOUD | THUNDER,           // 13 sunny intervals, thunderstorms
    CLOUD | DARK | RAIN,                    // 14 very cloudy, light rain
    CLOUD | DARK | RAIN | SNOW,             // 15 very cloudy, light sleet
    CLOUD | DARK | SNOW,                    // 16 very cloudy, light snow
    CLOUD | DARK | RAIN,                    // 17 intermittent rain
    CLOUD | DARK | RAIN | SNOW,             // 18 intermittent sleet
    CLOUD | DARK | SNOW,                    // 19 intermittent snow
    CLOUD | DARK | RAIN,                    // 20 rain
    CLOUD | DARK | RAIN | SNOW,             // 21 frequent sleet
    CLOUD | DARK | SNOW,                    // 22 heavy snow
    CLOUD | DARK | THUNDER,                 // 23 slight chance of thunderstorms
    CLOUD | DARK | THUNDER | RAIN,          // 24 thunderstorms
    CLOUD | DARK | THUNDER | RAIN,          // 25 heavy thunderstorms
    SUN_BEHIND | CLOUD,                     // 26 high clouds
    CLOUD,                                  // 27 stratus
    FOG,                                    // 28 fog
    SUN_BEHIND | CLOUD | RAIN,              // 29 sunny intervals, scattered showers
    SUN_BEHIND | CLOUD | SNOW,              // 30 ... scattered snow showers
    SUN_BEHIND | CLOUD | RAIN | SNOW,       // 31 ... scattered sleet
    SUN_BEHIND | CLOUD | RAIN,              // 32 ... some showers
    SUN_BEHIND | CLOUD | DARK | RAIN,       // 33 short sunny intervals, frequent rain
    SUN_BEHIND | CLOUD | DARK | SNOW,       // 34 ... frequent snow
    CLOUD | DARK,                           // 35 overcast and dry
    SUN_BEHIND | CLOUD | THUNDER,           // 36 sunny intervals, slightly stormy
    SUN_BEHIND | CLOUD | THUNDER | SNOW,    // 37 ... stormy, snow
    SUN_BEHIND | CLOUD | THUNDER | RAIN,    // 38 ... thunderstorms
    CLOUD | DARK | THUNDER | RAIN,          // 39 overcast, thunderstorms
    CLOUD | DARK | THUNDER | SNOW,          // 40 overcast, thunderstorms, snow
    CLOUD | DARK | THUNDER | RAIN,          // 41 overcast, heavy thunderstorms
    CLOUD | DARK | THUNDER | SNOW,          // 42 overcast, heavy thunderstorms, snow
  };
  int base = code % 100;
  return (base >= 0 && base <= 42) ? T[base] : CLOUD;
}

// Draws a weather icon in an s x s box at (x, y). Coordinates below are on a
// 24-unit grid and scaled to s.
static void drawIcon(int x, int y, int s, int code) {
  uint8_t p = iconParts(code);
  bool night = code > 100;
  auto X = [&](float u) { return x + (int)lroundf(u * s / 24); };
  auto Y = [&](float u) { return y + (int)lroundf(u * s / 24); };
  auto R = [&](float u) { return max(1, (int)lroundf(u * s / 24)); };
  bool below = p & (RAIN | SNOW | THUNDER);
  float dy = below ? 0 : 3;  // centre the cloud when nothing falls from it

  gfx->fillRect(x, y, s, s, C_BG);

  if (p & (SUN | SUN_BEHIND)) {
    bool big = p & SUN;
    float cx = big ? 12 : 8, cy = big ? 12 : 8 + dy / 2, r = big ? 5 : 3.5f;
    if (night) {
      float rm = big ? 8 : 5;
      gfx->fillCircle(X(cx), Y(cy), R(rm), C_LIGHT);
      gfx->fillCircle(X(cx + rm * 0.5f), Y(cy - rm * 0.35f), R(rm * 0.85f), C_BG);
    } else {
      gfx->fillCircle(X(cx), Y(cy), R(r), C_YELLOW);
      float r1 = r + 2, r2 = r + (big ? 5 : 3.5f);
      for (int k = 0; k < 8; k++) {
        float a = k * PI / 4, c = cosf(a), sn = sinf(a);
        gfx->drawLine(X(cx + c * r1), Y(cy + sn * r1), X(cx + c * r2), Y(cy + sn * r2), C_YELLOW);
      }
    }
  }

  if (p & CLOUD) {
    uint16_t col = (p & DARK) ? C_GREY : C_LIGHT;
    gfx->fillCircle(X(9), Y(13 + dy), R(4), col);
    gfx->fillCircle(X(14), Y(10.5f + dy), R(5), col);
    gfx->fillCircle(X(18.5f), Y(13.5f + dy), R(3.5f), col);
    gfx->fillRect(X(9), Y(13 + dy), X(18.5f) - X(9), Y(17 + dy) - Y(13 + dy), col);
    gfx->fillCircle(X(9), Y(13.5f + dy), R(3.5f), col);
  }

  if (p & FOG) {
    for (int k = 0; k < 4; k++) {
      float yy = 7 + k * 4, x0 = (k & 1) ? 6 : 3;
      gfx->fillRect(X(x0), Y(yy), X(x0 + 15) - X(x0), max(1, s / 12), C_LIGHT);
    }
  }

  // precipitation: up to 3 drops/flakes under the cloud (2 if there is lightning)
  if (p & (RAIN | SNOW)) {
    bool th = p & THUNDER;
    const float xs3[3] = {8, 12.5f, 17}, xs2[2] = {6.5f, 18.5f};
    const float *xs = th ? xs2 : xs3;
    int n = th ? 2 : 3;
    for (int k = 0; k < n; k++) {
      bool snow = (p & SNOW) && (!(p & RAIN) || k == 1);  // sleet: middle one is snow
      float yy = (k & 1) ? 20.5f : 19.5f;
      if (snow) {
        gfx->fillCircle(X(xs[k]), Y(yy + 1), R(1.2f), C_WHITE);
      } else {
        gfx->drawLine(X(xs[k] + 1), Y(yy - 0.5f), X(xs[k] - 0.5f), Y(yy + 3.5f), C_BLUE);
        gfx->drawLine(X(xs[k] + 2), Y(yy - 0.5f), X(xs[k] + 0.5f), Y(yy + 3.5f), C_BLUE);
      }
    }
  }

  if (p & THUNDER) {
    gfx->fillTriangle(X(14), Y(16), X(9.5f), Y(20.5f), X(13), Y(20.5f), C_YELLOW);
    gfx->fillTriangle(X(12.5f), Y(19.5f), X(15.5f), Y(19.5f), X(10.5f), Y(24), C_YELLOW);
  }
}

// "-3°" style temperature, Latin-1 degree sign.
static void fmtTemp(char *buf, size_t n, float t) {
  if (isnan(t)) snprintf(buf, n, "--\xB0");
  else snprintf(buf, n, "%ld\xB0", lroundf(t));
}

// Lower half: current conditions, then the next 12 hours in four 3-hour
// slots (icon, hour, rain chance) above a temperature line + rain bars,
// 20 px per hour.
static void drawWeather(time_t now) {
  char buf[48];
  struct tm tm;
  gfx->fillRect(0, WX_TOP, W, STATUS_Y - WX_TOP - 1, C_BG);

  if (!wx.valid) {
    text(4, WX_TOP + 4, F_TEXT, C_GREY, "loading weather...");
    return;
  }

  // --- current: icon, temperature, place, today's min/max and rain ---
  drawIcon(4, WX_TOP + 2, 32, wx.curIcon);
  fmtTemp(buf, sizeof buf, wx.curTemp);
  text(40, WX_TOP + 2, F_BIG, C_WHITE, buf);

  char name[24];
  utf8ToLatin1(WEATHER_NAME, name, sizeof name);
  snprintf(buf, sizeof buf, "%.*s", (W / 2 - 4) / F_HEADER.w, name);
  text(W - 4 - (int)strlen(buf) * F_HEADER.w, WX_TOP + 2, F_HEADER, C_LIGHT, buf);

  char lo[8], hi[8];
  fmtTemp(lo, sizeof lo, wx.dayMin);
  fmtTemp(hi, sizeof hi, wx.dayMax);
  if (!isnan(wx.dayPrecip) && wx.dayPrecip >= 0.1f)
    snprintf(buf, sizeof buf, "%s/%s %.1fmm", lo, hi, wx.dayPrecip);
  else
    snprintf(buf, sizeof buf, "%s/%s", lo, hi);
  text(W - 4 - (int)strlen(buf) * F_TEXT.w, WX_TOP + 2 + F_HEADER.h + 2, F_TEXT, C_GREY, buf);

  // --- 12 h window, starting at the current hour ---
  time_t h0 = now - (now - wx.start) % 3600;
  const int SLOT_Y = WX_TOP + 40;            // 200: icons + labels
  const int G_TOP = SLOT_Y + 28;             // 228: graph area
  const int G_BASE = STATUS_Y - 6;           // 301: bar baseline
  const int T_TOP = G_TOP + 13, T_BOT = G_BASE - 16;  // temperature line band

  for (int j = 0; j < 4; j++) {
    int x0 = j * 60;
    time_t t = h0 + j * 3 * 3600;
    drawIcon(x0 + 2, SLOT_Y, 24, icon3h(t));
    localtime_r(&t, &tm);
    snprintf(buf, sizeof buf, "%02dh", tm.tm_hour);
    text(x0 + 29, SLOT_Y, F_SMALL, C_LIGHT, buf);
    float pr = prob3h(t);
    if (!isnan(pr) && pr >= 5) {
      snprintf(buf, sizeof buf, "%ld%%", lroundf(pr));
      text(x0 + 29, SLOT_Y + F_SMALL.h, F_SMALL, C_BLUE, buf);
    }
    if (j > 0)  // faint slot separators through the graph
      for (int yy = G_TOP; yy < G_BASE; yy += 3) gfx->drawPixel(x0, yy, C_DARK);
  }

  // rain bars: 15 px per mm/h, capped at 30 px (2 mm/h = heavy)
  for (int i = 0; i < 12; i++) {
    float mm = precipHour(h0 + i * 3600);
    if (mm < 0.05f) continue;
    int hpx = max(2, min(30, (int)lroundf(mm * 15)));
    gfx->fillRect(i * 20 + 3, G_BASE - hpx, 14, hpx, C_BLUE);
  }
  gfx->drawFastHLine(0, G_BASE, W, C_DARK);

  // temperature line, scaled to the window's range (at least 4 °C tall)
  float t[12], tmin = 1e9, tmax = -1e9;
  for (int i = 0; i < 12; i++) {
    t[i] = tempHour(h0 + i * 3600);
    if (!isnan(t[i])) { tmin = min(tmin, t[i]); tmax = max(tmax, t[i]); }
  }
  if (tmax < tmin) return;  // no temperatures at all
  if (tmax - tmin < 4) { float c = (tmax + tmin) / 2; tmin = c - 2; tmax = c + 2; }
  auto ty = [&](float v) { return T_BOT - (int)lroundf((v - tmin) / (tmax - tmin) * (T_BOT - T_TOP)); };
  for (int i = 0; i + 1 < 12; i++) {
    if (isnan(t[i]) || isnan(t[i + 1])) continue;
    int xa = i * 20 + 10, xb = xa + 20, ya = ty(t[i]), yb = ty(t[i + 1]);
    gfx->drawLine(xa, ya, xb, yb, C_AMBER);
    gfx->drawLine(xa, ya + 1, xb, yb + 1, C_AMBER);
  }
  // value labels at the middle hour of each slot, above the line
  for (int j = 0; j < 4; j++) {
    int i = j * 3 + 1;
    if (isnan(t[i])) continue;
    fmtTemp(buf, sizeof buf, t[i]);
    int xc = i * 20 + 10, len = (int)strlen(buf) * F_SMALL.w;
    gfx->fillCircle(xc, ty(t[i]), 2, C_AMBER);
    // transparent text (no background box) so it doesn't cut into rain bars
    text(xc - len / 2, ty(t[i]) - F_SMALL.h - 2, F_SMALL, C_AMBER, buf, false);
  }
}

static void drawStatus() {
  char buf[64];
  struct tm tm;
  if (!fetchOk) {
    status("departures: fetch failed - retrying", C_RED);
  } else if (!wxFetchOk && (!wx.valid || time(nullptr) - wx.fetchedAt > 3600)) {
    status("weather: fetch failed - retrying", C_RED);
  } else {
    localtime_r(&lastFetchAt, &tm);
    strftime(buf, sizeof buf, "updated %H:%M:%S", &tm);
    status(buf, C_DARK);
  }
}

// ---------- touch: tap to turn the screen off / on ----------
// Only "is a finger on the screen" is needed, so this reads just the touch
// count, not coordinates. The board shipped with two touch chips:
//  - V1: CST328 at 0x1A (Waveshare demo driver Touch_CST328.cpp)
//  - V2 (from mid 2026): CST3530 at 0x58 (Waveshare esp_lcd_touch_cst3530)
// Whichever answers at boot is used.
enum TouchChip { TOUCH_NONE, TOUCH_CST328, TOUCH_CST3530 };
static TouchChip touchChip = TOUCH_NONE;
static bool screenOn = true;

static const uint8_t CST328_ADDR = 0x1A;
static const uint8_t CST3530_ADDR = 0x58;

// Sends `cmd` (register address or command bytes), then reads `n` bytes.
static bool i2cQuery(uint8_t addr, const uint8_t *cmd, size_t cmdLen, uint8_t *buf, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(cmd, cmdLen);
  if (Wire.endTransmission(false) != 0) return false;  // repeated start, no stop
  if (Wire.requestFrom((uint16_t)addr, n, true) != n) return false;
  for (size_t i = 0; i < n; i++) buf[i] = Wire.read();
  return true;
}

static bool i2cSend(uint8_t addr, const uint8_t *data, size_t n) {
  Wire.beginTransmission(addr);
  Wire.write(data, n);
  return Wire.endTransmission(true) == 0;
}

// CST3530: 4-byte commands. Info reply has 0xCA 0xCA at bytes 2..3. A touch
// report has 0xFF at byte 2 (else: no new report), the point count in the low
// nibble of byte 3 and the key count in the high nibble, then 5 bytes per
// key/point whose 5th byte's high nibble is the event (0 = finger lifted).
// Every read is followed by a "clear" command.
static const uint8_t CST3530_INFO[4]  = {0xD0, 0x03, 0x00, 0x00};
static const uint8_t CST3530_READ[4]  = {0xD0, 0x07, 0x00, 0x00};
static const uint8_t CST3530_CLEAR[4] = {0xD0, 0x00, 0x02, 0xAB};

static bool cst3530Detect() {
  uint8_t buf[4];
  for (int i = 0; i < 5; i++) {
    if (i2cQuery(CST3530_ADDR, CST3530_INFO, 4, buf, 4) && buf[2] == 0xCA && buf[3] == 0xCA)
      return true;
    delay(10);
  }
  return false;
}

// Fingers pressed per the latest report: >= 0, or -1 if there's no new report.
static int cst3530Touches() {
  uint8_t buf[32];
  bool ok = i2cQuery(CST3530_ADDR, CST3530_READ, 4, buf, sizeof buf);
  i2cSend(CST3530_ADDR, CST3530_CLEAR, 4);
  if (!ok || buf[2] != 0xFF) return -1;
  int points = buf[3] & 0x0F, keys = buf[3] >> 4, pressed = 0;
  if (points > 5) return -1;
  for (int i = 0; i < points; i++) {
    int idx = (keys + i) * 5 + 8;
    if (idx < (int)sizeof buf && (buf[idx] >> 4) != 0) pressed++;
  }
  return pressed;
}

// CST328: 16-bit registers. Switching to debug-info mode (0xD101) makes the
// chip info readable at 0xD1F4, where bytes 10..11 read 0xCACA; 0xD109 goes
// back to normal mode. The touch count is the low nibble of 0xD005, which is
// written back to 0 after each read.
static bool cst328Detect() {
  static const uint8_t infoMode[2] = {0xD1, 0x01}, info[2] = {0xD1, 0xF4},
                       normalMode[2] = {0xD1, 0x09};
  uint8_t buf[12] = {0};
  bool ok = i2cSend(CST328_ADDR, infoMode, 2) && i2cQuery(CST328_ADDR, info, 2, buf, 12);
  i2cSend(CST328_ADDR, normalMode, 2);
  return ok && buf[10] == 0xCA && buf[11] == 0xCA;
}

// Fingers on the screen, or -1 if none (the CST328 has no explicit lift report).
static int cst328Touches() {
  static const uint8_t countReg[2] = {0xD0, 0x05}, clear[3] = {0xD0, 0x05, 0x00};
  uint8_t n = 0;
  bool ok = i2cQuery(CST328_ADDR, countReg, 2, &n, 1);
  i2cSend(CST328_ADDR, clear, 3);
  n &= 0x0F;
  return (ok && n >= 1 && n <= 5) ? n : -1;
}

static void touchBegin() {
  Wire.begin(PIN_TP_SDA, PIN_TP_SCL, 100000);
  pinMode(PIN_TP_RST, OUTPUT);  // reset is active low
  digitalWrite(PIN_TP_RST, HIGH);
  delay(10);
  digitalWrite(PIN_TP_RST, LOW);
  delay(10);
  digitalWrite(PIN_TP_RST, HIGH);
  delay(120);  // chip boot time

  if (cst3530Detect()) touchChip = TOUCH_CST3530;
  else if (cst328Detect()) touchChip = TOUCH_CST328;
  if (touchChip != TOUCH_NONE) {
    Serial.printf("touch: %s found\n", touchChip == TOUCH_CST3530 ? "CST3530" : "CST328");
    return;
  }
  // Diagnostics: list whatever answers on the touch bus.
  Serial.print("touch: no known touch chip, tap-to-sleep disabled. I2C devices on SDA=1/SCL=3:");
  for (uint8_t a = 1; a < 127; a++) {
    Wire.beginTransmission(a);
    if (Wire.endTransmission() == 0) Serial.printf(" 0x%02X", a);
  }
  Serial.println();
}

// > 0: finger(s) down, 0: finger lifted, -1: nothing new.
static int touchState() {
  return touchChip == TOUCH_CST3530 ? cst3530Touches()
       : touchChip == TOUCH_CST328  ? cst328Touches()
       : -1;
}

// The backlight is the screen's light source: off = really dark, and almost
// all of the display's power. Fetching and drawing go on in the background,
// so the screen is up to date the moment it comes back.
static void setScreen(bool on) {
  screenOn = on;
  digitalWrite(PIN_LCD_BL, on ? HIGH : LOW);
  Serial.printf("touch: screen %s\n", on ? "on" : "off");
}

// Toggles the screen once per tap: on the press, never on the lift. A tap
// ends with a lift report, or after 300 ms without news (chips that don't
// report lifts), so a held finger still counts once.
static void handleTouch() {
  static bool down = false;
  static uint32_t lastContact = 0;
  uint32_t ms = millis();
  int st = touchState();
  if (st > 0) {
    if (!down) setScreen(!screenOn);
    down = true;
    lastContact = ms;
  } else if (st == 0 || (down && ms - lastContact > 300)) {
    down = false;
  }
}

// ---------- main ----------
void setup() {
  Serial.begin(115200);
  Serial.printf("PSRAM: %u bytes\n", ESP.getPsramSize());
  pinMode(PIN_LCD_BL, OUTPUT);
  digitalWrite(PIN_LCD_BL, HIGH);

  if (!gfx->begin()) Serial.println("gfx->begin() failed");
  gfx->fillScreen(C_BG);
  gfx->setTextWrap(false);
  gfx->setTextSize(1);
  touchBegin();

  status("connecting WiFi...", C_GREY);
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) delay(200);

  status("syncing time...", C_GREY);
  configTzTime(TZ_POSIX, "ch.pool.ntp.org", "pool.ntp.org");
  while (time(nullptr) < 1700000000) delay(200);

  gfx->fillScreen(C_BG);
  gfx->drawFastHLine(4, DIVIDER_Y, W - 8, C_DARK);
}

void loop() {
  static uint32_t nextFetch = 0, nextWeather = 0, nextDraw = 0;
  static time_t wxDrawnHour = 0;
  handleTouch();
  uint32_t ms = millis();
  time_t now = time(nullptr);

  if ((int32_t)(ms - nextFetch) >= 0) {
    fetchOk = WiFi.status() == WL_CONNECTED && fetchDepartures();
    if (fetchOk) lastFetchAt = time(nullptr);
    nextFetch = ms + (fetchOk ? FETCH_EVERY_MS : 5000);
    nextDraw = ms;  // redraw right away
  }
  if ((int32_t)(ms - nextWeather) >= 0) {
    wxFetchOk = WiFi.status() == WL_CONNECTED && fetchWeather(now);
    if (wxFetchOk) wxDirty = true;
    nextWeather = millis() + (wxFetchOk ? WEATHER_EVERY_MS : 60000);
    nextDraw = ms;
  }
  if ((int32_t)(ms - nextDraw) >= 0) {
    drawDepartures(now);
    // The weather area is redrawn only when new data arrives or the hour
    // rolls over (the 12 h window moves), so it never flickers otherwise.
    time_t hour = now / 3600;
    if (wxDirty || hour != wxDrawnHour) {
      drawWeather(now);
      wxDrawnHour = hour;
      wxDirty = false;
    }
    drawStatus();
    nextDraw = ms + REDRAW_EVERY_MS;
  }
  delay(20);
}
