// Surf forecast display — 7.5" Waveshare panel, ESP32 e-Paper Driver Board
// Diamond Bay, Sorrento — 7 days x 3 time slots (10am / 1pm / 4pm)
//
// Expects JSON shaped like (height/wind direction split from magnitude so the
// firmware can draw a chunky rotated arrow instead of a compass letter):
// {
//   "days": [
//     { "date": "2026-09-17", "slots": [
//         {"time":"10 AM","rating":7,"height":"2.1m","heightDirection":"SW","energy":"1234 kJ","wind":"15 kmh","windDirection":"SW"},
//         {"time":"1 PM","rating":8,"height":"2.1m","heightDirection":"SW","energy":"1234 kJ","wind":"15 kmh","windDirection":"SW"},
//         {"time":"4 PM","rating":6,"height":"2.1m","heightDirection":"SW","energy":"1234 kJ","wind":"15 kmh","windDirection":"SW"}
//     ]},
//     ... 7 days total
//   ]
// }
// If the endpoint's field names differ, only parseForecast() needs to change.

#include <GxEPD2_BW.h>
#include <Fonts/FreeSansBold12pt7b.h>
#include <Fonts/FreeSansBold9pt7b.h>
#include <Fonts/FreeSans9pt7b.h>
#include <WiFi.h>
#include <WiFiManager.h>
#include <Preferences.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include "time.h"

// ---- Waveshare ESP32 Driver Board pins (confirmed working) ----
#define CS   15
#define DC   27
#define RST  26
#define BUSY 25

#if defined(ESP32)
#define USE_HSPI_FOR_EPD
#include <SPI.h>
static SPIClass hspi(HSPI);
#endif

// Swap for GxEPD2_750_GDEY075T7 if that's what your demo test confirmed instead
GxEPD2_BW<GxEPD2_750_T7, GxEPD2_750_T7::HEIGHT> display(
  GxEPD2_750_T7(/*CS=*/ CS, /*DC=*/ DC, /*RST=*/ RST, /*BUSY=*/ BUSY));

// ---- Config ----
// WiFi credentials and the spot live in NVS, set through the setup portal —
// nothing to hardcode, nothing to reflash when a customer moves the display.
const char* relayBaseUrl = "https://gimmiesickvis.com/api/eink-forecast";
const char* ntpServer    = "pool.ntp.org";
const char* tzMelbourne  = "AEST-10AEDT,M10.1.0,M4.1.0/3";
const char* portalName   = "GimmieSickVis";
const char* brandText    = "gimmiesickvis.com";

Preferences prefs;
String spotId   = "diamond"; // slug the API knows; unknown ones fall back to Diamond Bay
String spotName = "";        // display name, straight from the API response

// The portal opens on its own when WiFi won't connect. To change spot or
// network on a unit that IS connecting, press RST twice: the first press
// restarts it, the second lands while this flag is still set.
// (GPIO0 can't do this — held low at reset the ESP32 enters its serial
// bootloader and never runs the sketch.)
RTC_DATA_ATTR uint32_t portalFlag;
const uint32_t PORTAL_MAGIC = 0x50525450; // "PRTP"
const int PORTAL_TIMEOUT_S = 180;         // don't hold a battery unit open forever

const uint64_t SLEEP_SECONDS = 3600;
const int SLOTS_PER_DAY = 3;
const int RATING_HIGHLIGHT_THRESHOLD = 8; // rating >= this gets the double border

// ---- Layout constants ----
const int PANEL_W = 800;
const int PANEL_H = 480;
const int TITLE_H = 55;
const int DAY_HEADER_H = 35;
const int FOOTER_H = 20;
const int COL_W = 114; // last column absorbs the remainder

struct Slot {
  String time;
  int rating = -1;
  String height;
  String heightDirection;
  String energy;
  String wind;
  String windDirection;
};

struct DayForecast {
  String dateLabel;
  String dayName;
  String monthDayISO;
  Slot slots[SLOTS_PER_DAY];
};

DayForecast week[7];

void setup() {
  Serial.begin(115200);
  Serial.println("setup");

#if defined(ESP32) && defined(USE_HSPI_FOR_EPD)
  hspi.begin(13, 12, 14, 15);
  display.epd2.selectSPI(hspi, SPISettings(4000000, MSBFIRST, SPI_MODE0));
#endif

  display.init(115200);

  bool portalRequested = (portalFlag == PORTAL_MAGIC);
  portalFlag = PORTAL_MAGIC; // goToSleep() clears it once a run completes

  prefs.begin("gsv", false);
  spotId = prefs.getString("spot", spotId);

  WiFiManagerParameter spotParam("spot", "Dive spot id", spotId.c_str(), 24);
  WiFiManager wm;
  wm.addParameter(&spotParam);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  // "Setup" (/param) lets someone change the spot alone — the WiFi page would
  // make them retype the network password just to move the display.
  std::vector<const char*> menu = {"wifi", "param", "info", "sep", "restart", "exit"};
  wm.setMenu(menu); // takes a non-const reference, so it needs a named vector
  // Save on the portal's Save button, not on a clean exit: a customer who
  // changes the spot and wanders off would otherwise lose it to the timeout.
  wm.setSaveParamsCallback([&spotParam]() { saveSpot(spotParam.getValue()); });
  wm.setAPCallback([](WiFiManager *) {
    drawError("SETUP: join WiFi network GimmieSickVis");
  });

  bool connected = portalRequested ? wm.startConfigPortal(portalName)
                                   : wm.autoConnect(portalName);

  // Portal timed out. If credentials are already stored, that's a spot-only
  // edit (or nobody touched it), so join with what's saved rather than
  // burning an hour on an error screen.
  if (!connected && WiFi.SSID().length() > 0) {
    WiFi.begin();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 15000) {
      delay(250);
    }
    connected = (WiFi.status() == WL_CONNECTED);
  }

  if (!connected) {
    drawError("No WiFi - press RST twice to set up");
    goToSleep();
    return;
  }

  saveSpot(spotParam.getValue());

  configTzTime(tzMelbourne, ntpServer);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, 10000)) {
    drawError("No time sync");
    goToSleep();
    return;
  }

  buildWeekStructure(timeinfo);

  String payload;
  if (fetchForecast(payload)) {
    parseForecast(payload);
    drawWeek(timeinfo);
  } else {
    drawError("Fetch failed");
  }

  WiFi.disconnect(true);
  goToSleep();
}

// Persist a spot only when it's a real change — NVS writes are finite.
void saveSpot(const char* value) {
  if (value == nullptr || strlen(value) == 0) return; // field left blank: keep what's stored
  if (spotId == value) return;
  spotId = value;
  prefs.putString("spot", spotId);
  Serial.print("spot -> ");
  Serial.println(spotId);
}

void loop() {
  // never reached — ESP32 restarts from setup() after each deep sleep wake
}

// Rolling 7-day window starting today (column 0 = today)
void buildWeekStructure(struct tm &timeinfo) {
  time_t now = mktime(&timeinfo);

  char isoBuf[11];
  char dayBuf[3];
  char nameBuf[4];
  for (int i = 0; i < 7; i++) {
    time_t dayTime = now + (i * 86400L);
    struct tm dayTm;
    localtime_r(&dayTime, &dayTm);
    strftime(isoBuf, sizeof(isoBuf), "%Y-%m-%d", &dayTm);
    strftime(dayBuf, sizeof(dayBuf), "%d", &dayTm);
    strftime(nameBuf, sizeof(nameBuf), "%a", &dayTm);
    week[i].monthDayISO = String(isoBuf);
    week[i].dateLabel = String(dayBuf);
    week[i].dayName = String(nameBuf);
    week[i].dayName.toUpperCase(); // e.g. "Thu" -> "THU"
  }
}

bool fetchForecast(String &payloadOut) {
  String relayUrl = String(relayBaseUrl) + "?spot=" + spotId;

  HTTPClient http;
  http.begin(relayUrl);
  int code = http.GET();
  bool ok = (code == 200);
  if (ok) payloadOut = http.getString();
  http.end();
  return ok;
}

void parseForecast(const String &payload) {
  // ArduinoJson 7: JsonDocument grows as needed, so there's no capacity to guess
  // (DynamicJsonDocument still compiles, but it's deprecated and ignores the number).
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, payload);
  if (err) {
    Serial.print("JSON parse error: ");
    Serial.println(err.c_str());
    return;
  }

  spotName = String((const char*)(doc["name"] | ""));

  JsonArray days = doc["days"].as<JsonArray>();
  for (JsonObject day : days) {
    const char* date = day["date"] | "";

    for (int i = 0; i < 7; i++) {
      if (week[i].monthDayISO == String(date)) {
        JsonArray slots = day["slots"].as<JsonArray>();
        int s = 0;
        for (JsonObject slot : slots) {
          if (s >= SLOTS_PER_DAY) break;
          week[i].slots[s].time = String((const char*)(slot["time"] | ""));
          week[i].slots[s].rating = slot["rating"] | -1;
          week[i].slots[s].height = String((const char*)(slot["height"] | ""));
          week[i].slots[s].heightDirection = String((const char*)(slot["heightDirection"] | ""));
          week[i].slots[s].energy = String((const char*)(slot["energy"] | ""));
          week[i].slots[s].wind = String((const char*)(slot["wind"] | ""));
          week[i].slots[s].windDirection = String((const char*)(slot["windDirection"] | ""));
          s++;
        }
        break;
      }
    }
  }
}

void rotatePoint(float x, float y, float angleDeg, int cx, int cy, int &outX, int &outY) {
  float rad = angleDeg * PI / 180.0;
  float rx = x * cos(rad) - y * sin(rad);
  float ry = x * sin(rad) + y * cos(rad);
  outX = cx + round(rx);
  outY = cy + round(ry);
}

int directionToAngle(const String &dirIn) {
  // Compass letters are meteorological "from" directions (e.g. "SW" = coming
  // from the southwest). The arrow should show where it's heading, not where
  // it came from, so each angle here is the letter's compass bearing + 180.
  String dir = dirIn;
  dir.trim();
  dir.toUpperCase();
  if (dir == "N") return 180;
  if (dir == "NE") return 225;
  if (dir == "E") return 270;
  if (dir == "SE") return 315;
  if (dir == "S") return 0;
  if (dir == "SW") return 45;
  if (dir == "W") return 90;
  if (dir == "NW") return 135;
  return -1; // unrecognized — skip drawing
}

// Chunky filled arrow: a wide arrowhead triangle plus a short stem rectangle,
// rotated around (cx, cy) to point in the compass direction given by angleDeg
// (0 = north/up, clockwise). Drawn as 3 filled triangles since GxEPD2 has no
// native polygon fill.
void drawDirectionArrow(int cx, int cy, float angleDeg) {
  if (angleDeg < 0) return; // unrecognized direction — draw nothing rather than guess

  int tipX, tipY, rWingX, rWingY, lWingX, lWingY;
  int stemTLx, stemTLy, stemTRx, stemTRy, stemBRx, stemBRy, stemBLx, stemBLy;

  rotatePoint(0, -12, angleDeg, cx, cy, tipX, tipY);
  rotatePoint(7, 4, angleDeg, cx, cy, rWingX, rWingY);
  rotatePoint(-7, 4, angleDeg, cx, cy, lWingX, lWingY);

  rotatePoint(-2.5, 4, angleDeg, cx, cy, stemTLx, stemTLy);
  rotatePoint(2.5, 4, angleDeg, cx, cy, stemTRx, stemTRy);
  rotatePoint(2.5, 12, angleDeg, cx, cy, stemBRx, stemBRy);
  rotatePoint(-2.5, 12, angleDeg, cx, cy, stemBLx, stemBLy);

  display.fillTriangle(tipX, tipY, rWingX, rWingY, lWingX, lWingY, GxEPD_BLACK);
  display.fillTriangle(stemTLx, stemTLy, stemTRx, stemTRy, stemBRx, stemBRy, GxEPD_BLACK);
  display.fillTriangle(stemTLx, stemTLy, stemBRx, stemBRy, stemBLx, stemBLy, GxEPD_BLACK);
}

void drawRatingBadge(int x1, int y, int rating) {
  const int badgeSize = 18;
  int bx = x1 - 6 - badgeSize;
  int by = y - 14;

  display.fillRect(bx, by, badgeSize, badgeSize, GxEPD_BLACK);

  if (rating >= RATING_HIGHLIGHT_THRESHOLD) {
    // double border: an outer unfilled ring with a gap, for ideal-conditions emphasis
    display.drawRect(bx - 4, by - 4, badgeSize + 8, badgeSize + 8, GxEPD_BLACK);
  }

  display.setFont(&FreeSansBold9pt7b);
  display.setTextColor(GxEPD_WHITE);
  char ratingBuf[4];
  if (rating >= 0) snprintf(ratingBuf, sizeof(ratingBuf), "%d", rating);
  else snprintf(ratingBuf, sizeof(ratingBuf), "-");

  int16_t bx2, by2; uint16_t bw2, bh2;
  display.getTextBounds(ratingBuf, 0, 0, &bx2, &by2, &bw2, &bh2);
  display.setCursor(bx + (badgeSize - bw2) / 2, by + badgeSize - 4);
  display.print(ratingBuf);
  display.setTextColor(GxEPD_BLACK);
}

void drawWeek(struct tm &timeinfo) {
  char updatedBuf[20];
  strftime(updatedBuf, sizeof(updatedBuf), "UPDATED: %H:%M", &timeinfo);

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    // title bar
    display.drawLine(0, TITLE_H, PANEL_W, TITLE_H, GxEPD_BLACK);
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(15, 35);
    String title = spotName.length() ? ("Dive forecast: " + spotName) : String("Dive forecast");
    title.toUpperCase();
    display.print(title);

    int16_t bx, by; uint16_t bw, bh;
    display.setFont(&FreeSans9pt7b);
    display.getTextBounds(updatedBuf, 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(PANEL_W - bw - 15, 32);
    display.print(updatedBuf);

    int contentTop = TITLE_H + DAY_HEADER_H;
    int contentBottom = PANEL_H - FOOTER_H;
    int slotHeight = (contentBottom - contentTop) / SLOTS_PER_DAY;

    for (int i = 0; i < 7; i++) {
      int x0 = i * COL_W;
      int x1 = (i == 6) ? PANEL_W : x0 + COL_W;

      if (i > 0) {
        display.drawLine(x0, TITLE_H, x0, contentBottom, GxEPD_BLACK);
      }

      // day header — invert to black for today's column (always index 0)
      char headerText[10];
      snprintf(headerText, sizeof(headerText), "%s %s", week[i].dayName.c_str(), week[i].dateLabel.c_str());
      display.setFont(&FreeSansBold9pt7b);
      display.getTextBounds(headerText, 0, 0, &bx, &by, &bw, &bh);

      if (i == 0) {
        display.fillRect(x0, TITLE_H, x1 - x0, DAY_HEADER_H, GxEPD_BLACK);
        display.setTextColor(GxEPD_WHITE);
      } else {
        display.setTextColor(GxEPD_BLACK);
      }
      display.setCursor(x0 + ((x1 - x0) - bw) / 2, TITLE_H + 24);
      display.print(headerText);
      display.setTextColor(GxEPD_BLACK);
      display.drawLine(x0, contentTop, x1, contentTop, GxEPD_BLACK);

      // 3 time slots
      int badgeCenterX = x1 - 15; // matches drawRatingBadge's badge center, so arrows line up under it
      for (int s = 0; s < SLOTS_PER_DAY; s++) {
        int slotY = contentTop + s * slotHeight;
        Slot &slot = week[i].slots[s];

        display.setFont(&FreeSansBold9pt7b);
        display.setCursor(x0 + 6, slotY + 20);
        display.print(slot.time);

        drawRatingBadge(x1, slotY + 20, slot.rating);

        display.setFont(&FreeSans9pt7b);
        display.setCursor(x0 + 6, slotY + 44);
        display.print(slot.height);
        drawDirectionArrow(badgeCenterX, slotY + 37, directionToAngle(slot.heightDirection));

        display.setCursor(x0 + 6, slotY + 66);
        display.print(slot.energy);

        display.setCursor(x0 + 6, slotY + 88);
        display.print(slot.wind);
        drawDirectionArrow(badgeCenterX, slotY + 81, directionToAngle(slot.windDirection));
      }
    }

    // footer
    display.drawLine(0, contentBottom, PANEL_W, contentBottom, GxEPD_BLACK);
    display.setFont(&FreeSans9pt7b);

    display.getTextBounds(brandText, 0, 0, &bx, &by, &bw, &bh);
    display.setCursor(PANEL_W - bw - 15, PANEL_H - 5);
    display.print(brandText);

  } while (display.nextPage());

  display.powerOff();
}

void drawError(const char* msg) {
  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);
    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(15, 40);
    display.print(msg);
  } while (display.nextPage());
  display.powerOff();
}

void goToSleep() {
  portalFlag = 0; // a completed run means the next boot isn't a double-reset
  esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}
