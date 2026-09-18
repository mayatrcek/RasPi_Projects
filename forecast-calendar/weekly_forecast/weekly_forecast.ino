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
const char* ntpServer2    = "time.google.com";   // pool.ntp.org needs DNS; these are backups
const char* ntpServer3    = "time.cloudflare.com";
const int NTP_TIMEOUT_MS  = 30000;               // first sync after a cold boot can be slow
const char* tzMelbourne  = "AEST-10AEDT,M10.1.0,M4.1.0/3";
const char* portalName   = "GimmieSickVis";
const char* brandText    = "gimmiesickvis.com";

// ---- Setup portal copy ----
// The portal runs in AP mode with no internet, so the spot list has to ship in
// the firmware. Source of truth is lib/data/regions.ts in gimmie-SICK-vis-vic;
// regenerate it from that file's `id`/`name` pairs as <option> tags (see
// the plan notes, or just grep regions.ts) when the site gains a spot.
// Drift is harmless: the API validates the id and falls back to Diamond Bay.
static const char PORTAL_INTRO[] =
  "<div id='gsvintro' style='background:#FFFAEF;border:2px solid #161310;box-shadow:4px 4px 0 #161310;padding:12px;margin-bottom:18px'>"
  "<h3 style='margin:0 0 8px;font-size:1.1rem'>Dive forecast display</h3>"
  "<p style='margin:0 0 8px'>This panel shows 7 days of dive conditions for one spot on the "
  "Victorian coast, and refreshes itself every hour.</p>"
  "<p style='margin:0'>Tap your network in the list below, enter its password, choose a dive "
  "spot, then press <b>Update</b>.</p>"
  "</div>";

// The spot menu. WiFiManager only renders <input>, so the real control is this
// raw-HTML block and the saved field is a hidden input it writes into; the
// script seeds the menu from whatever is stored. With JS off nothing moves and
// the stored spot survives, which is the safe failure.
static const char SPOT_PICKER[] =
  "<label for='spotsel' style='display:block;margin-bottom:4px'><b>3. Dive spot</b></label>"
  "<select id='spotsel' onchange=\"document.getElementById('spot').value=this.value\">"
  "<optgroup label='Surf Coast'><option value='bells'>Bells Beach</option><option value='winki'>Winkipop</option><option value='janjuc'>Jan Juc</option><option value='torquay'>Torquay Point</option><option value='roadknight'>Point Roadknight</option><option value='anglesea'>Anglesea</option><option value='lorne'>Lorne</option><option value='apollo'>Apollo Bay</option></optgroup><optgroup label='Bellarine'><option value='13th'>13th Beach</option><option value='barwon'>Barwon Heads</option><option value='oceangrove'>Ocean Grove</option><option value='lonsdale'>Point Lonsdale</option></optgroup><optgroup label='Mornington Peninsula'><option value='pointnepean'>Point Nepean (buoy)</option><option value='portsea'>Portsea Back Beach</option><option value='diamond'>Diamond Bay</option><option value='sorrento'>Sorrento Back Beach</option><option value='rye'>Rye Back Beach</option><option value='gunnamatta'>Gunnamatta</option><option value='schanck'>Cape Schanck</option><option value='flinders'>Flinders</option><option value='pointleo'>Point Leo</option></optgroup><optgroup label='Phillip Island'><option value='woolamai'>Cape Woolamai</option><option value='smiths'>Smiths Beach</option><option value='surfbeach'>Surf Beach</option><option value='pyramid'>Pyramid Rock</option><option value='express'>Express Point</option><option value='summerland'>Summerland</option><option value='ycw'>YCW / Cat Bay</option></optgroup><optgroup label='East Coast / Gippsland'><option value='capepat'>Cape Paterson</option><option value='inverloch'>Inverloch</option><option value='venus'>Venus Bay</option><option value='waratah'>Waratah Bay</option><option value='sandypt'>Sandy Point</option><option value='walkerville'>Walkerville</option></optgroup><optgroup label='Wilsons Promontory'><option value='tonguept'>Tongue Point</option><option value='whiskybay'>Whisky Bay</option><option value='squeaky'>Squeaky Beach</option><option value='normanbay'>Norman Bay (Tidal River)</option><option value='shellback'>Shellback Island</option><option value='oberon'>Oberon Bay</option><option value='glennie'>Great Glennie Island</option><option value='cleft'>Cleft Island (Skull Rock)</option><option value='anser'>Anser Island</option><option value='kanowna'>Kanowna Island</option><option value='rodondo'>Rodondo Island</option><option value='waterloobay'>Waterloo Bay</option><option value='refugecove'>Refuge Cove</option><option value='sealerscove'>Sealers Cove</option></optgroup><optgroup label='Far West / Shipwreck Coast'><option value='portcampbell'>Port Campbell</option><option value='princetown'>Princetown</option><option value='warrnambool'>Warrnambool (Logans)</option><option value='portfairy'>Port Fairy</option><option value='portland'>Portland</option></optgroup><optgroup label='Port Phillip'><option value='fort'>South Channel Fort</option><option value='blairgowrie'>Blairgowrie (bay)</option><option value='ryepier'>Rye Pier</option><option value='sorrentopier'>Sorrento Pier</option><option value='portseapier'>Portsea Pier</option><option value='portseahole'>Portsea Hole</option><option value='popeseye'>Popes Eye</option><option value='chinamans'>Chinaman's Hat</option><option value='lonsdalewall'>Lonsdale Wall</option><option value='queenscliffpier'>Queenscliff Pier</option><option value='stleonards'>St Leonards Pier</option><option value='portarlington'>Portarlington Pier</option><option value='morningtonpier'>Mornington Pier</option><option value='ricketts'>Ricketts Point</option><option value='cerberus'>HMVS Cerberus (Black Rock)</option><option value='williamstown'>Williamstown (The Dell)</option></optgroup><optgroup label='Western Port'><option value='flinderspier'>Flinders Pier</option><option value='cowes'>Cowes Jetty</option><option value='stonypoint'>Stony Point Pier</option><option value='crawfish'>Crawfish Rock</option><option value='rhyll'>Rhyll Jetty</option><option value='newhaven'>Newhaven Pier (San Remo)</option><option value='tortoise'>Tortoise Head (French Is.)</option><option value='corinella'>Corinella Pier</option></optgroup>"
  "</select>"
  "<p style='font-size:13px;color:#3A332A;margin:10px 0 0'>"
  "<b style='color:#A8200D'>*</b> Needed the first time only. Leave the network and password "
  "blank to keep the current network and change the spot on its own.</p>"
  // Seed on DOM ready, not inline: the hidden input renders after this block, so
  // seeding at parse time finds nothing and the menu opens on the first spot.
  "<script>document.addEventListener('DOMContentLoaded',function(){"
  "var h=document.getElementById('spot'),s=document.getElementById('spotsel');"
  "if(h&&s)s.value=h.value;"
  // the intro renders inside the form, below the password; lift it above the network list
  "var i=document.getElementById('gsvintro'),w=document.querySelector('.wrap'),t=w?w.querySelector('h3'):null;"
  "if(i&&w)w.insertBefore(i,t?t.nextSibling:w.firstChild);});</script>";

// Portal styling. Injected after WiFiManager's own <style> (WiFiManager.cpp:1281),
// so these win. Palette is OVERWORLD from the site's app/overworld.css; the fonts
// are its system fallbacks, because the portal serves an AP with no internet and
// no webfont can load. Button labels are rewritten here rather than in the
// library — that ties them to WiFiManager 2.0.17's markup (wm_strings_en.h:45),
// and a markup change blanks them rather than making them wrong.
static const char PORTAL_CSS[] =
  // Straight to the form: the menu page is one tap of nothing.
  "<script>if(location.pathname=='/')location.replace('/wifi');</script>"
  "<style>"
  "body{background:#F2EAD6;color:#161310;font-family:system-ui,-apple-system,'Segoe UI',Roboto,sans-serif;font-size:16px}"
  ".wrap{width:100%;max-width:520px;padding:0 16px;box-sizing:border-box}"
  "h1{font-family:ui-monospace,SFMono-Regular,Menlo,monospace;text-transform:uppercase;letter-spacing:.04em;font-size:1.6rem;margin:18px 0 2px}"
  ".wrap>h3{color:#3A332A;font-weight:400;font-size:1rem;margin:0 0 18px}"
  "button,input[type='submit']{background:#2E5DD6;color:#FFFAEF;border:2px solid #161310;border-radius:0;"
  "box-shadow:4px 4px 0 #161310;min-height:48px;font-weight:700;font-size:1.1rem;line-height:1.3}"
  "button:active,input[type='submit']:active{transform:translate(2px,2px);box-shadow:2px 2px 0 #161310;opacity:1 !important}"
  "input,select{background:#FFFAEF;color:#161310;border:2px solid #161310;border-radius:0;min-height:44px;font-size:16px;width:100%}"
  "select{padding-right:28px}"
  "input[type='checkbox']{width:auto;min-height:0;margin-right:6px}"
  "label[for='showpass']{font-size:.9rem}"
  "input:focus-visible,select:focus-visible,button:focus-visible{outline:0;box-shadow:0 0 0 3px #2E5DD6}"
  ".msg{background:#FFFAEF;border:2px solid #161310;border-left-width:5px;border-left-color:#2E5DD6;border-radius:0}"
  "a{color:#161310}a:hover{color:#2E5DD6}"
  // Field labels and the submit button are library strings, so they get rewritten
  // here; see the note above about this tying us to 2.0.17's markup.
  "label[for='s'],label[for='p']{font-size:0;display:block;margin-top:14px}"
  "label[for='s']::before{content:'1. WiFi network ';font-size:1rem;font-weight:700}"
  "label[for='p']::before{content:'2. WiFi password ';font-size:1rem;font-weight:700}"
  "label[for='s']::after,label[for='p']::after{content:'*';font-size:1rem;font-weight:700;color:#A8200D}"
  "form[action='/wifisave'] button[type='submit']{font-size:0;margin-top:18px}"
  "form[action='/wifisave'] button[type='submit']::after{content:'Update';font-size:1.1rem}"
  // only reachable if the redirect above doesn't run
  "form[action='/wifi'] button{font-size:0}"
  "form[action='/wifi'] button::after{content:'Set up the display';font-size:1.1rem}"
  "</style>";

Preferences prefs;
String spotId   = "diamond"; // slug the API knows; unknown ones fall back to Diamond Bay
String spotName = "";        // display name, straight from the API response

// The portal opens on its own when WiFi won't connect. To reach it on a working
// unit: press EN (reset) twice — once to restart it, again while it's awake.
// The flag lives in NVS, not RTC memory: EN cuts power to the RTC domain, so
// anything kept there reads back empty and the second press looks like the first.
// GPIO0/BOOT can't do this either, as a wake on it boots with the pin still low,
// which is the ESP32's serial-bootloader strap.
const char* PORTAL_FLAG = "portalpend";
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

  prefs.begin("gsv", false);
  spotId = prefs.getString("spot", spotId);

  // Set on every boot, cleared once a run finishes. Still set = the last boot
  // never reached sleep, i.e. EN was pressed again mid-run.
  bool portalRequested = prefs.getBool(PORTAL_FLAG, false);
  prefs.putBool(PORTAL_FLAG, true);
  Serial.print("wake cause: ");
  Serial.print(esp_sleep_get_wakeup_cause());
  Serial.print("  portal requested: ");
  Serial.println(portalRequested ? "yes" : "no");

  // The 5th argument lands inside the <input> tag: the saved field is hidden and
  // the <select> in SPOT_PICKER writes into it.
  WiFiManagerParameter spotParam("spot", "", spotId.c_str(), 24, "type='hidden'");
  WiFiManagerParameter introBlock(PORTAL_INTRO);
  WiFiManagerParameter spotPicker(SPOT_PICKER);

  WiFiManager wm;
  wm.setTitle("Gimmie Sick Vis");
  wm.setCustomHeadElement(PORTAL_CSS);
  wm.addParameter(&introBlock); // first, so it renders above the fields
  wm.addParameter(&spotPicker); // the menu, then the hidden field it feeds
  wm.addParameter(&spotParam);
  wm.setConfigPortalTimeout(PORTAL_TIMEOUT_S);
  // Everything on one page: setMenu() moves params onto their own page whenever
  // "param" is listed, so leaving it out keeps them under the WiFi fields.
  std::vector<const char*> menu = {"wifi"};
  wm.setMenu(menu); // takes a non-const reference, so it needs a named vector
  // Save on the portal's Save button, not on a clean exit: a customer who
  // changes the spot and wanders off would otherwise lose it to the timeout.
  // A blank SSID means WiFiManager skips the wifi save (keeping the stored
  // network), so nothing closes the portal. Shorten the timeout on save instead:
  // ~20s after the last page load it closes and the panel redraws.
  wm.setSaveParamsCallback([&]() {
    saveSpot(spotParam.getValue());
    wm.setConfigPortalTimeout(20);
  });
  wm.setAPCallback([](WiFiManager *) { drawSetupScreen(); });

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
    drawError("No WiFi - press EN twice to set up");
    goToSleep();
    return;
  }

  saveSpot(spotParam.getValue());

  // The portal leaves the radio in AP+STA. SNTP can pick the AP interface and
  // never hear a reply, so drop back to station-only before asking for time.
  WiFi.mode(WIFI_STA);
  Serial.print("IP: ");
  Serial.println(WiFi.localIP());

  configTzTime(tzMelbourne, ntpServer, ntpServer2, ntpServer3);
  struct tm timeinfo;
  if (!getLocalTime(&timeinfo, NTP_TIMEOUT_MS)) {
    Serial.println("NTP failed - no reply on UDP 123 within timeout");
    drawError("No time sync - check the router allows NTP");
    goToSleep();
    return;
  }
  Serial.println(&timeinfo, "time: %Y-%m-%d %H:%M:%S");

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

// Shown while the setup portal is open. drawError() stays the one-line path.
void drawSetupScreen() {
  const char* steps[] = {
    "1.  On a phone or laptop, join the WiFi network:  GimmieSickVis",
    "2.  A setup page opens by itself. If it doesn't, browse to  192.168.4.1",
    "3.  Tap 'Choose your WiFi network', or 'Choose your dive spot' to move it.",
    "4.  Press Save. This panel redraws within a minute, then updates hourly.",
  };

  display.setFullWindow();
  display.firstPage();
  do {
    display.fillScreen(GxEPD_WHITE);

    display.setFont(&FreeSansBold12pt7b);
    display.setTextColor(GxEPD_BLACK);
    display.setCursor(15, 35);
    display.print("DISPLAY SETUP");
    display.drawLine(0, TITLE_H, PANEL_W, TITLE_H, GxEPD_BLACK);

    display.setFont(&FreeSans9pt7b);
    display.setCursor(15, TITLE_H + 40);
    display.print("This display needs a WiFi network and a dive spot before it can show a forecast.");

    int y = TITLE_H + 90;
    for (int i = 0; i < 4; i++) {
      display.setCursor(30, y);
      display.print(steps[i]);
      y += 34;
    }

    display.setCursor(15, y + 20);
    display.print("Showing: ");
    display.print(spotName.length() ? spotName : spotId);

    int contentBottom = PANEL_H - FOOTER_H;
    display.drawLine(0, contentBottom, PANEL_W, contentBottom, GxEPD_BLACK);
    display.setCursor(15, PANEL_H - 5);
    display.print("This page closes after 3 minutes. Press EN twice to reopen it.");

    int16_t bx, by; uint16_t bw, bh;
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
  // A completed run means the next boot isn't a double-press.
  prefs.putBool(PORTAL_FLAG, false);
  esp_sleep_enable_timer_wakeup(SLEEP_SECONDS * 1000000ULL);
  esp_deep_sleep_start();
}
