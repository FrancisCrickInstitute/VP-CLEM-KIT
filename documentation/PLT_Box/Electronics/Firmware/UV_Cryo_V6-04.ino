// UV_Cryo_V6 — Milestone 6.04
// ESP32 WROOM (Arduino-ESP32 v3.x)
//
// What’s in here:
// - I2C @ 100 kHz (SDA=23, SCL=22) driving OLED (0x3D) + seesaw encoder (0x36)
// - OLED info screen only (no splash), updates ≥ 1 Hz  [SSD1306 128x64]
// - PWM on IO32 using LEDC @ 120 Hz; encoder adjusts duty immediately
// - Cooler MOSFET on IO33 via MCP1416 gate driver; turned ON after boot
// - Door interlock: magnetic switch on IO12 (INPUT_PULLUP); HIGH=open → LED forced OFF
// - Encoder A/B replaced by Adafruit I2C QT Rotary Encoder (seesaw 0x36); button toggles SD logging
// - SPI (SCK=5, MISO=19, MOSI=18), CS: TC1=16, TC2=17, TC3=2, SD=4
// - Three MAX31855 thermocouples (hardware SPI)
// - Web GUI with simple plots; phone time sync; mDNS: http://PLTBox.local
// - SD logging: CSV once/min; “LOG” badge while actively logging, “OK” if card idle, “N/A” if absent
//   * Start logging: fully re-inits SPI and re-mounts SD (handles hot re-insert)
//   * While mounted: every 2 s check SD presence (cardType + open(“/”)); update OLED immediately if lost; stop logging too if active
//   * Safe to eject after you stop logging
//
// Libraries: Adafruit_GFX, Adafruit_SSD1306, Adafruit_seesaw, Adafruit_MAX31855, SD, WiFi, WebServer, ESPmDNS

#include <Arduino.h>
#include <Wire.h>
#include <SPI.h>
#include <SD.h>
#include <WiFi.h>
#include <WebServer.h>
#include <ESPmDNS.h>
#include <time.h>
#include <sys/time.h>

#include <Adafruit_GFX.h>
#include <Adafruit_SSD1306.h>
#include <Adafruit_seesaw.h>
#include <seesaw_neopixel.h>
#include <Adafruit_MAX31855.h>

// ---------------- Pins ----------------
const int LED_PWM_PIN = 32;
const int I2C_SDA = 23, I2C_SCL = 22;
const int SPI_SCK = 5, SPI_MISO = 19, SPI_MOSI = 18;
const int TC_CS1 = 16, TC_CS2 = 17, TC_CS3 = 2, SD_CS = 4;
const int COOLER_PIN = 33;
const int DOOR_SWITCH_PIN = 12;   // magnetic door switch; INPUT_PULLUP → HIGH=open, LOW=closed

// ---------------- I2C ----------------
const uint32_t I2C_CLOCK_HZ = 100000;   // 100 kHz
const uint32_t I2C_TIMEOUT_MS = 20;

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);
uint8_t oledAddr = 0x3D;
bool hasOLED = false;

// ---------------- Door interlock ----------------
// Magnetic switch on DOOR_SWITCH_PIN (INPUT_PULLUP).
// Switch open (door open) → pin HIGH; switch closed (door closed) → pin LOW.

// ---------------- MAX31855 ----------------
Adafruit_MAX31855 tc1(TC_CS1), tc2(TC_CS2), tc3(TC_CS3);

// ---------------- PWM (LEDC) ----------------
const uint32_t LEDC_FREQ = 120;
const uint8_t  LEDC_RES_BITS = 10;

int intendedDuty = 0;                   // 0..100 set by encoder
volatile int dutyPercent = 0;           // applied (0 when door open)
const int DUTY_STEP = 5;
bool startupConfigActive = true;      // true while startup time is being set; LED output forced OFF

// Door state
bool doorOpen = false;

// ---------------- I2C Rotary Encoder (Adafruit seesaw, 0x36) ----------------
Adafruit_seesaw ss;
bool hasEncoder = false;
int32_t lastEncPos = 0;
#define SS_SWITCH 24   // seesaw GPIO pin for the push-button
#define SS_NEOPIX_PIN 6
#define SS_NEOPIXELS 1
seesaw_NeoPixel encoderPixel = seesaw_NeoPixel(SS_NEOPIXELS, SS_NEOPIX_PIN, NEO_GRB + NEO_KHZ800);
bool hasEncoderPixel = false;

// Button
bool btnPressFlag = false;
unsigned long lastBtnHandledMs = 0;
const unsigned long BTN_DEBOUNCE_MS = 50;

// ---------------- WiFi / Web ----------------
const char* ssid = "PltBox";
const char* password = "PltBox!";
WebServer server(80);

// ---------------- Timekeeping ----------------
bool timeIsSet = false;
int  tzOffsetMin = 0;
bool     softClockValid = false;
int64_t  epochBase = 0;
uint32_t millisBase = 0;
String isoNow();  // fwd decl

// ---------------- Data cache ----------------
double t1 = NAN, t2 = NAN, t3 = NAN;
double lastGoodT1 = NAN, lastGoodT2 = NAN, lastGoodT3 = NAN;  // fallback for CSV logging

// ---------------- SD / Logging ----------------
File logFile;
bool sdOK = false;            // last SD.begin() result
bool sdPresent = false;       // open("/") success
bool sdLoggingEnabled = false; // user intent; toggled by encoder button
bool lastWriteOK = false;
unsigned long lastLogMs = 0;
const unsigned long LOG_INTERVAL_MS = 60000;

// Fast SD-loss check while logging
unsigned long lastSdHealthMs = 0;
const unsigned long SD_HEALTH_INTERVAL_MS = 2000;  // 2s

// UI refresh hint
volatile bool needOLEDRefresh = false;

// ----------- Forward declarations -----------
void i2cBeginOnCustomPins();
bool i2cPing(uint8_t addr7);
void ensureAllCSHigh();
double readTCFiltered(Adafruit_MAX31855 &tc);
void applyPWMDuty();
void updateEffectiveDutyFromDoor();
void handleSeesawEncoder();
void runStartupTimeSetup();
void drawOLED();
void updateEncoderPixel();
void updateDoorState();
bool sdReinitBusAndMount(uint32_t hz = 8000000);
bool sdTryInit();
void ensureLogOpen();
void closeLogFile();
void logLine();
String makeFilename();

void handleRoot();
void handleData();
void handleTime();
String displayTimeNow();
static long long extractJsonInt64(const String& body, const char* key, bool* ok);
static long extractJsonInt(const String& body, const char* key, bool* ok);

// =================== I2C ===================

/** Initialize Wire on custom SDA/SCL, clock, and timeout. */
void i2cBeginOnCustomPins() {
  Wire.begin(I2C_SDA, I2C_SCL);
  Wire.setClock(I2C_CLOCK_HZ);
  Wire.setTimeOut(I2C_TIMEOUT_MS);
}

/** Return true if a device ACKs at 7-bit addr. */
bool i2cPing(uint8_t addr7) {
  Wire.beginTransmission(addr7);
  return (Wire.endTransmission() == 0);
}

// =================== SPI/MAX31855 ===================

/** De-select all SPI slaves to avoid MISO contention before any transaction. */
void ensureAllCSHigh() {
  digitalWrite(TC_CS1, HIGH);
  digitalWrite(TC_CS2, HIGH);
  digitalWrite(TC_CS3, HIGH);
  digitalWrite(SD_CS,  HIGH);
}

/** Read one MAX31855 with basic sanity filtering; return NAN on failure.
 *  readCelsius() and readError() each trigger a separate SPI transaction, so they must
 *  NOT be mixed: readCelsius() already returns NAN for any fault condition, making a
 *  separate readError() call both redundant and misleading (it reads fresh, unrelated data).
 *  One short retry covers the case where the poll lands mid-conversion (~70-100 ms period). */
double readTCFiltered(Adafruit_MAX31855 &tc) {
  ensureAllCSHigh();
  double c = tc.readCelsius();   // returns NAN on any fault (OC, short-to-VCC/GND)
  if (!isnan(c) && c > -200 && c < 1350) return c;
  delay(15);
  c = tc.readCelsius();
  if (!isnan(c) && c > -200 && c < 1350) return c;
  return NAN;
}


// =================== Door interlock ===================

/** Read the magnetic door switch and update doorOpen; enforce PWM if state changed. */
void updateDoorState() {
  bool prev = doorOpen;
  doorOpen = (digitalRead(DOOR_SWITCH_PIN) == HIGH);  // pull-up: HIGH = switch open = door open
  if (doorOpen != prev) {
    Serial.println(doorOpen ? "[DOOR] OPEN — forcing LEDs OFF." : "[DOOR] CLOSED — restoring PWM.");
    updateEffectiveDutyFromDoor();
  }
}

// =================== PWM / Controls ===================

/** Write the current dutyPercent (0..100) to LEDC. */
void applyPWMDuty() {
  uint32_t raw = map(dutyPercent, 0, 100, 0, (1u << LEDC_RES_BITS) - 1);
  ledcWrite(LED_PWM_PIN, startupConfigActive ? 0 : raw);
  updateEncoderPixel();
}

/** Enforce door interlock (0 when open; intendedDuty when closed) and update PWM if changed. */
void updateEffectiveDutyFromDoor() {
  int newEffective = doorOpen ? 0 : intendedDuty;
  if (newEffective != dutyPercent) {
    dutyPercent = newEffective;
    applyPWMDuty();
    needOLEDRefresh = true;
  }
}

/** Poll the seesaw I2C encoder for position changes and button presses; adjust intendedDuty. */
void handleSeesawEncoder() {
  if (!hasEncoder) return;

  // Encoder: each detent = 1 count on the seesaw encoder
  int32_t pos = ss.getEncoderPosition();
  int32_t delta = lastEncPos - pos;
  lastEncPos = pos;
  if (delta != 0) {
    intendedDuty = constrain(intendedDuty + (int)(delta * DUTY_STEP), 0, 100);
    updateEffectiveDutyFromDoor();
  }

  // Button: detect falling edge (active LOW with internal pull-up)
  static bool lastBtnState = true;
  bool btnState = (bool)ss.digitalRead(SS_SWITCH);
  if (!btnState && lastBtnState) btnPressFlag = true;
  lastBtnState = btnState;
}


// =================== Time ===================

/** Return ISO-like "YYYY-MM-DD HH:MM:SS" using system or software clock; "NA" if unavailable. */
String isoNow() {
  time_t now; time(&now);
  if (now >= 10) now += tzOffsetMin * 60;
  else if (softClockValid) { uint32_t dms = millis() - millisBase; now = (time_t)(epochBase + (dms / 1000)); now += tzOffsetMin * 60; }
  else return String("NA");
  struct tm t; gmtime_r(&now, &t);
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

/** Return display date-time as "YYYY/MM/DD HH:MM:SS"; "NA" if unavailable. */
String displayTimeNow() {
  time_t now; time(&now);
  if (now >= 10) now += tzOffsetMin * 60;
  else if (softClockValid) {
    uint32_t dms = millis() - millisBase;
    now = (time_t)(epochBase + (dms / 1000));
    now += tzOffsetMin * 60;
  } else {
    return String("NA");
  }
  struct tm t; gmtime_r(&now, &t);
  char buf[25];
  snprintf(buf, sizeof(buf), "%04d/%02d/%02d %02d:%02d:%02d",
           t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
  return String(buf);
}

// =================== OLED ===================
/** Render OLED: title, 2-column sensor table, and date-time line. */
void drawOLED() {
  if (!hasOLED || !i2cPing(oledAddr)) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  // Header (centered)
  String title = "PLT Box Control";
  int16_t tx, ty; uint16_t tw, th;
  display.getTextBounds(title, 0, 0, &tx, &ty, &tw, &th);
  int titleX = (SCREEN_WIDTH - (int)tw) / 2;
  if (titleX < 0) titleX = 0;
  display.setCursor(titleX, 0);
  display.print("PLT Box Control");

  // Table grid
  display.drawFastHLine(0, 9, 128, SSD1306_WHITE);
  display.drawFastHLine(0, 22, 128, SSD1306_WHITE);
  display.drawFastHLine(0, 35, 128, SSD1306_WHITE);
  display.drawFastHLine(0, 48, 128, SSD1306_WHITE);
  display.drawFastVLine(64, 10, 38, SSD1306_WHITE);

  String sdLabel = "N/A";
  if (sdLoggingEnabled && logFile) sdLabel = "LOG";
  else if (sdPresent) sdLabel = "OK";

  // Row 1
  display.setCursor(2, 13);
  display.print("LED: "); display.print(dutyPercent); display.print("%");
  display.setCursor(66, 13);
  display.print("SD: "); display.print(sdLabel);

  // Row 2
  display.setCursor(2, 26);
  display.print("T1=");
  if (isnan(t1)) display.print("N/A");
  else { display.print(t1, 1); display.print("C"); }
  display.setCursor(66, 26);
  display.print("T2=");
  if (isnan(t2)) display.print("N/A");
  else { display.print(t2, 1); display.print("C"); }

  // Row 3
  display.setCursor(2, 39);
  display.print("T3=");
  if (isnan(t3)) display.print("N/A");
  else { display.print(t3, 1); display.print("C"); }
  display.setCursor(66, 39);
  display.print(doorOpen ? "Opened" : "Closed");

  // Date-time (centered)
  String dts = displayTimeNow();
  int16_t dx, dy; uint16_t dw, dh;
  display.getTextBounds(dts, 0, 0, &dx, &dy, &dw, &dh);
  int dtX = (SCREEN_WIDTH - (int)dw) / 2;
  if (dtX < 0) dtX = 0;
  display.setCursor(dtX, 55);
  display.print(dts);

  display.display();
}

/** Set encoder neopixel to red when effective output is OFF/0%, blue when ON. */
void updateEncoderPixel() {
  if (!hasEncoderPixel) return;
  bool ledIsOn = !startupConfigActive && (dutyPercent > 0);
  uint32_t color = ledIsOn
    ? encoderPixel.Color(0, 0, 64)   // blue
    : encoderPixel.Color(64, 0, 0);  // red
  encoderPixel.setPixelColor(0, color);
  encoderPixel.show();
}

/** Encoder-driven startup time setup: rotate to change value, click to confirm each field. */
void runStartupTimeSetup() {
  int year = 2026, month = 1, day = 1, hour = 13, minute = 0;
  if (!hasEncoder) {
    Serial.println("[TIME] No encoder; applying default startup time.");
  } else {
    const char* labels[5] = {"Year", "Month", "Day", "Hour", "Min"};
    int* vals[5] = {&year, &month, &day, &hour, &minute};
    const int mins[5] = {2020, 1, 1, 0, 0};
    const int maxs[5] = {2099, 12, 31, 23, 59};

    bool lastBtn = true;
    int32_t lastPosLocal = ss.getEncoderPosition();

    for (int field = 0; field < 5; ++field) {
      bool done = false;
      while (!done) {
        int32_t pos = ss.getEncoderPosition();
        int32_t delta = lastPosLocal - pos;  // clockwise increases value
        lastPosLocal = pos;
        if (delta != 0) {
          *vals[field] += (int)delta;
          if (*vals[field] < mins[field]) *vals[field] = mins[field];
          if (*vals[field] > maxs[field]) *vals[field] = maxs[field];

          if (field == 2) {
            int mdays = 31;
            if (month == 4 || month == 6 || month == 9 || month == 11) mdays = 30;
            else if (month == 2) {
              bool leap = ((year % 4 == 0) && ((year % 100 != 0) || (year % 400 == 0)));
              mdays = leap ? 29 : 28;
            }
            if (day > mdays) day = mdays;
          }
        }

        bool btn = (bool)ss.digitalRead(SS_SWITCH);
        if (!btn && lastBtn) {
          done = true;
          delay(120);
        }
        lastBtn = btn;

        if (hasOLED) {
          display.clearDisplay();
          display.setTextSize(1);
          display.setTextColor(SSD1306_WHITE);
          display.setCursor(0, 0);
          display.print("Set startup time");
          display.setCursor(0, 12);
          display.print("Field: "); display.print(labels[field]);
          display.setCursor(0, 24);
          char dt[24];
          snprintf(dt, sizeof(dt), "%04d/%02d/%02d %02d:%02d", year, month, day, hour, minute);
          display.print(dt);
          display.setCursor(0, 40);
          display.print("Turn=change");
          display.setCursor(0, 50);
          display.print("Click=next");
          display.display();
        }
        delay(20);
      }
    }
  }

  struct tm t = {};
  t.tm_year = year - 1900;
  t.tm_mon  = month - 1;
  t.tm_mday = day;
  t.tm_hour = hour;
  t.tm_min  = minute;
  t.tm_sec  = 0;
  time_t epoch = mktime(&t);

  if (epoch > 0) {
    struct timeval tv;
    tv.tv_sec = epoch;
    tv.tv_usec = 0;
    settimeofday(&tv, nullptr);
    timeIsSet = true;
    tzOffsetMin = 0;
    epochBase = tv.tv_sec;
    millisBase = millis();
    softClockValid = true;
    Serial.printf("[TIME] Startup set: %s\n", displayTimeNow().c_str());
  }
}

// =================== SD / Logging ===================

/** Fully reset SPI bus and (re)mount SD at a conservative speed; return true if mounted and root opens. */
bool sdReinitBusAndMount(uint32_t hz) {
  ensureAllCSHigh();
  SPI.end();
  delay(2);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);
  delay(2);

  // Note: SD.begin() on ESP32 ignores the hz param, but we leave it for clarity.
  bool ok = SD.begin(SD_CS, SPI /*, hz*/);
  if (!ok) { sdOK = false; sdPresent = false; return false; }
  sdOK = true;

  File root = SD.open("/");
  if (!root) { sdPresent = false; return false; }
  root.close();

  uint8_t ctype = SD.cardType();
  Serial.print("[SD] Re-mounted. Type="); Serial.print(ctype);
  Serial.println(" (0=None,1=MMC,2=SD,3=SDHC)");
  sdPresent = true;
  return true;
}

/** On-demand init: fully re-init SPI+SD, probe writability, and treat failures as unusable. */
bool sdTryInit() {
  if (!sdReinitBusAndMount(8000000)) return false;

  ensureAllCSHigh();
  File probe = SD.open("/.__probe.txt", FILE_WRITE);
  if (!probe) {
    Serial.println("[SD] Mount OK but cannot create files (write-protect or socket issue).");
    sdPresent = false; sdOK = false;
    return false;
  }
  size_t w = probe.println("ok");
  probe.flush();
  probe.close();
  if (w == 0) {
    Serial.println("[SD] Mount OK but write failed (contact/RO?).");
    sdPresent = false; sdOK = false;
    return false;
  }
  SD.remove("/.__probe.txt");

  sdOK = true; sdPresent = true;
  return true;
}

/** Pick a filename using timestamp if available else LOG_XXXX.CSV (first free index). */
String makeFilename() {
  if (timeIsSet) {
    time_t now; time(&now);
    if (now >= 10) {
      struct tm t; gmtime_r(&now, &t);
      char name[32];
      snprintf(name, sizeof(name), "/%04d%02d%02d_%02d%02d%02d.csv",
               t.tm_year + 1900, t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
      return String(name);
    }
  }
  for (int i = 0; i < 100; i++) {  // limit iterations: each SD.exists() is a blocking SPI call
    char name[20]; snprintf(name, sizeof(name), "/LOG_%04d.CSV", i);
    if (!SD.exists(name)) return String(name);
  }
  return String("/LOG_LAST.CSV");
}

/** Open CSV and write header once; retry after a quick re-mount if the first open fails. */
void ensureLogOpen() {
  if (!sdOK || !sdPresent) return;
  if (logFile) return;

  ensureAllCSHigh();
  String fname = makeFilename();
  logFile = SD.open(fname, FILE_WRITE);
  if (!logFile) {
    Serial.println("[SD] Failed to open log file. Retrying mount and open...");
    if (sdReinitBusAndMount(8000000)) {
      ensureAllCSHigh();
      fname = makeFilename();
      logFile = SD.open(fname, FILE_WRITE);
    }
  }

  if (logFile) {
    logFile.println("timestamp,pwm,enabled,T1_C,T2_C,T3_C,Door");
    logFile.flush();
    lastWriteOK = true;
    Serial.print("[SD] Logging to "); Serial.println(fname);
    needOLEDRefresh = true;
  } else {
    Serial.println("[SD] Failed to open log file after retry.");
  }
}

/** Flush & close the current CSV file. */
void closeLogFile() {
  if (logFile) { logFile.flush(); logFile.close(); logFile = File(); }
}

/** Append one CSV row; if write fails, stop logging immediately and close file. */
void logLine() {
  if (!sdLoggingEnabled || !logFile) return;

  ensureAllCSHigh();   // avoid MISO fights

  String ts = isoNow();
  String doorStr = doorOpen ? "OPEN" : "CLOSED";
  String line = ts + "," + String(dutyPercent) + ",1,";
  line += (isnan(t1) ? (isnan(lastGoodT1) ? String("") : String(lastGoodT1, 2)) : String(t1, 2)); line += ",";
  line += (isnan(t2) ? (isnan(lastGoodT2) ? String("") : String(lastGoodT2, 2)) : String(t2, 2)); line += ",";
  line += (isnan(t3) ? (isnan(lastGoodT3) ? String("") : String(lastGoodT3, 2)) : String(t3, 2)); line += ",";
  line += doorStr;

  size_t wrote = logFile.println(line);
  if (wrote == 0) {
    Serial.println("[SD] Write failed — stopping logging.");
    sdLoggingEnabled = false;
    closeLogFile();
    lastWriteOK = false;
    needOLEDRefresh = true;
    return;
  }
  logFile.flush();
  lastWriteOK = true;
  needOLEDRefresh = true;
}


// =================== Web / Time ===================

/** Serve minimal dashboard with plots + “Sync time” button. */
void handleRoot() {
  String html = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>PLT Box Monitor</title>
<style>
 body{font-family:system-ui,Arial,sans-serif;margin:16px}
 .row{display:flex;gap:16px;flex-wrap:wrap}
 .card{border:1px solid #ccc;border-radius:8px;padding:12px;min-width:280px}
 canvas{width:320px;height:160px;border:1px solid #ddd;border-radius:4px}
 .big{font-size:18px;margin:6px 0}
 button{padding:6px 10px;border-radius:6px;border:1px solid #aaa;background:#f5f5f5}
</style>
</head><body>
<h2>PLT Box Monitor</h2>
<p id="summary" class="big">Loading…</p>
<p>Time: <span id="time">—</span> <button onclick="sendTime()">Sync time</button></p>

<div class="row">
  <div class="card"><h3>PWM (%)</h3><canvas id="pwm"></canvas></div>
  <div class="card"><h3>T1 (°C)</h3><canvas id="t1"></canvas></div>
  <div class="card"><h3>T2 (°C)</h3><canvas id="t2"></canvas></div>
  <div class="card"><h3>T3 (°C)</h3><canvas id="t3"></canvas></div>
</div>

<script>
class MiniPlot {
  constructor(id, ymin, ymax){ this.c=document.getElementById(id); this.y=[];
    this.maxN=60; this.ymin=ymin; this.ymax=ymax; this.ctx=this.c.getContext('2d'); }
  push(val){ const v=(val===null||val===undefined)?NaN:Number(val);
    this.y.push(v); if(this.y.length>this.maxN) this.y.shift(); this.draw(); }
  draw(){
    const w=this.c.width, h=this.c.height, ctx=this.ctx;
    ctx.clearRect(0,0,w,h);
    ctx.strokeStyle='#aaa'; ctx.beginPath(); ctx.moveTo(30,10); ctx.lineTo(30,h-20); ctx.lineTo(w-10,h-20); ctx.stroke();
    ctx.fillStyle='#555'; ctx.font='12px sans-serif'; ctx.fillText(this.ymax, 2, 14); ctx.fillText(this.ymin, 2, h-22);
    const left=30, top=10, right=w-10, bottom=h-20; const W=right-left, H=bottom-top;
    ctx.strokeStyle='#0066cc'; ctx.fillStyle='#0066cc'; ctx.beginPath(); let first=true;
    for(let i=0;i<this.y.length;i++){
      const x=left + (i/(this.maxN-1))*W; const v=this.y[i]; if(isNaN(v)) { first=true; continue; }
      const y=top + (1-((v-this.ymin)/(this.ymax-this.ymin)))*H;
      if(first){ ctx.moveTo(x,y); first=false; } else ctx.lineTo(x,y);
    } ctx.stroke();
    for(let i=0;i<this.y.length;i++){
      const v=this.y[i]; if(isNaN(v)) continue;
      const x=left + (i/(this.maxN-1))*W;
      const y=top + (1-((v-this.ymin)/(this.ymax-this.ymin)))*H;
      ctx.beginPath(); ctx.arc(x,y,2,0,2*Math.PI); ctx.fill();
    }
  }
}
const plots={ pwm:new MiniPlot('pwm',0,100),
  t1:new MiniPlot('t1',-20,50), t2:new MiniPlot('t2',-20,50), t3:new MiniPlot('t3',-20,50) };
async function tick(){
  try{
    const r=await fetch('/data'); const j=await r.json();
    const door = j.door ? "Door Open" : "Door Closed";
    document.getElementById('summary').innerText =
      `PWM: ${j.pwm}% ${j.enabled?'[ON]':'[OFF]'} | T1: ${j.t1??'—'}°C  T2: ${j.t2??'—'}°C  T3: ${j.t3??'—'}°C  | [${door}]`;
    document.getElementById('time').innerText = j.time ?? '—';
    plots.pwm.push(j.pwm); plots.t1.push(j.t1); plots.t2.push(j.t2); plots.t3.push(j.t3);
  }catch(e){ document.getElementById('summary').innerText='Waiting for data…'; }
}
async function sendTime(){
  const epochMs = Date.now();
  const tzMin   = new Date().getTimezoneOffset() * -1;
  try{
    await fetch('/time', {method:'POST', headers:{'Content-Type':'application/json'},
      body: JSON.stringify({epoch_ms: epochMs, tz_offset_min: tzMin})});
  }catch(e){}
}
setInterval(tick,1000); tick(); sendTime();
</script>
</body></html>
)HTML";
  server.send(200, "text/html", html);
}

/** Return current values as JSON for the web UI (polled once/sec), including door state & time. */
void handleData() {
  String json = "{";
  json += "\"pwm\":" + String(dutyPercent) + ",";
  json += "\"enabled\":true,";
  json += "\"t1\":" + (isnan(t1) ? String("null") : String(t1, 1)) + ",";
  json += "\"t2\":" + (isnan(t2) ? String("null") : String(t2, 1)) + ",";
  json += "\"t3\":" + (isnan(t3) ? String("null") : String(t3, 1)) + ",";
  json += "\"door\":" + String(doorOpen ? "true" : "false") + ",";
  json += "\"time\":\"" + isoNow() + "\"";
  json += "}";
  server.send(200, "application/json", json);
}

/** Tiny JSON int64 extractor for /time POST body. */
static long long extractJsonInt64(const String& body, const char* key, bool* ok) {
  int i = body.indexOf(key); if (i < 0) { if(ok)*ok=false; return 0; }
  i = body.indexOf(':', i);  if (i < 0) { if(ok)*ok=false; return 0; }
  int start = i + 1; while (start < (int)body.length() && (body[start]==' '||body[start]=='\"')) start++;
  int end = start;  while (end < (int)body.length()) { char c = body[end]; if ((c>='0'&&c<='9')||c=='-') end++; else break; }
  String num = body.substring(start, end);
  char* ep = nullptr; long long v = strtoll(num.c_str(), &ep, 10);
  if (ok) *ok = (ep != num.c_str());
  return v;
}

/** 32-bit variant backed by extractJsonInt64. */
static long extractJsonInt(const String& body, const char* key, bool* ok) {
  bool lok=false; long long v = extractJsonInt64(body, key, &lok);
  if (ok) *ok = lok; return (long)v;
}

/** Accept epoch_ms & tz_offset_min, set system & software clocks, reply OK. */
void handleTime() {
  if (server.hasArg("plain")) {
    String body = server.arg("plain");
    bool ok1=false, ok2=false;
    long long epochMs = extractJsonInt64(body, "epoch_ms", &ok1);
    long tzMinLocal   = extractJsonInt(body,  "tz_offset_min", &ok2);
    if (ok1) {
      struct timeval tv;
      tv.tv_sec  = (time_t)(epochMs / 1000LL);
      tv.tv_usec = (suseconds_t)((epochMs % 1000LL) * 1000LL);
      settimeofday(&tv, nullptr);
      timeIsSet = true;
      if (ok2) tzOffsetMin = (int)tzMinLocal;
      epochBase  = tv.tv_sec; millisBase = millis(); softClockValid = true;
      Serial.print("[TIME] Synced: "); Serial.print(isoNow());
      Serial.print(" (tz "); Serial.print(tzOffsetMin); Serial.println(" min)");
      server.send(200, "text/plain", "OK"); return;
    }
  }
  server.send(400, "text/plain", "Bad request");
}

// =================== Setup / Loop ===================

/** Initialize all subsystems (I²C, OLED, SPI/SD try, seesaw encoder, door switch, PWM, Wi-Fi/web) and draw once. */
void setup() {
  Serial.begin(115200);
  delay(50);

  i2cBeginOnCustomPins();
  delay(30);

  oledAddr = 0x3D;
  if (i2cPing(oledAddr)) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, oledAddr)) {
      Serial.printf("OLED begin failed (0x%02X ACKed).\n", oledAddr);
    } else {
      hasOLED = true;
      display.clearDisplay();
      display.setTextSize(2);
      display.setTextColor(SSD1306_WHITE);
      display.setCursor(0, 0);
      display.println("PLT Box");
      display.setTextSize(1);
      display.println("OLED OK");
      display.display();
      Serial.printf("[OLED] SSD1306 init OK at 0x%02X.\n", oledAddr);
    }
  } else {
    Serial.println("[OLED] SSD1306 not found at 0x3D.");
  }

  // SPI + CS defaults
  pinMode(TC_CS1, OUTPUT); digitalWrite(TC_CS1, HIGH);
  pinMode(TC_CS2, OUTPUT); digitalWrite(TC_CS2, HIGH);
  pinMode(TC_CS3, OUTPUT); digitalWrite(TC_CS3, HIGH);
  pinMode(SD_CS,  OUTPUT); digitalWrite(SD_CS,  HIGH);
  SPI.begin(SPI_SCK, SPI_MISO, SPI_MOSI);

  // Try SD once at boot; if present, write a single BOOT line
  sdOK = SD.begin(SD_CS, SPI);
  if (sdOK) { File root = SD.open("/"); if (root) { sdPresent = true; root.close(); } }
  Serial.println((sdOK && sdPresent) ? "[SD] SD init OK (format FAT32)." : "[SD] No SD detected at boot.");

  // VL53 replaced by magnetic door switch — handled in updateDoorState()

  // PWM
  ledcAttach(LED_PWM_PIN, LEDC_FREQ, LEDC_RES_BITS);
  dutyPercent = intendedDuty; applyPWMDuty();

  // I2C rotary encoder (Adafruit seesaw at 0x36)
  if (ss.begin(0x36)) {
    hasEncoder = true;
    ss.pinMode(SS_SWITCH, INPUT_PULLUP);
    ss.setEncoderPosition(0);
    lastEncPos = 0;
    encoderPixel.begin(0x36);
    encoderPixel.setBrightness(40);
    hasEncoderPixel = true;
    updateEncoderPixel();
    Serial.println("[ENC] Seesaw encoder OK at 0x36.");
  } else {
    Serial.println("[ENC] Seesaw encoder not found at 0x36.");
  }

  // Magnetic door switch
  pinMode(DOOR_SWITCH_PIN, INPUT_PULLUP);

  // User time setup at startup (default 2026/01/01 13:00).
  runStartupTimeSetup();

  // Startup configuration is complete; enable normal LED/interlock behavior.
  startupConfigActive = false;
  updateDoorState();
  updateEffectiveDutyFromDoor();

  // WiFi AP + mDNS + web
  WiFi.persistent(false);  // don't restore SSID/password from flash
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("[AP] IP: "); Serial.println(ip);
  if (MDNS.begin("PLTBox")) { MDNS.addService("http", "tcp", 80); Serial.println("[mDNS] Hostname: http://PLTBox.local"); }
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/time", HTTP_POST, handleTime);
  server.begin();

  // Cooler ON after all subsystems initialised
  pinMode(COOLER_PIN, OUTPUT);
  digitalWrite(COOLER_PIN, HIGH);
  Serial.println("[COOLER] Cooler ON (GPIO33 HIGH).");

  Serial.printf("[BOOT] Duty: %d%%\n", intendedDuty);

  // Apply startup duty to hardware.
  dutyPercent = intendedDuty;
  applyPWMDuty();

  // First reads + draw
  t1 = readTCFiltered(tc1); if (!isnan(t1)) lastGoodT1 = t1;
  t2 = readTCFiltered(tc2); if (!isnan(t2)) lastGoodT2 = t2;
  t3 = readTCFiltered(tc3); if (!isnan(t3)) lastGoodT3 = t3;
  updateDoorState();
  drawOLED();
}

/** Main loop: handle encoder/button, HTTP, SD health (if card mounted), sensors+OLED each second, CSV once/min. */
void loop() {
  updateDoorState();  // safety: evaluate interlock every loop pass
  handleSeesawEncoder();

  // Button press: on-demand SD init when starting logging
  do {
    if (!btnPressFlag) break;
    btnPressFlag = false;
    unsigned long now = millis();
    if (now - lastBtnHandledMs < BTN_DEBOUNCE_MS) break;
    lastBtnHandledMs = now;

    if (!sdLoggingEnabled) {
      // Always reinit bus + mount on each start to handle hot re-insert
      if (!sdTryInit()) {
        Serial.println("[SD] Cannot START logging (no SD / RO / init failed).");
        needOLEDRefresh = true;
        break;
      }
      ensureLogOpen();
      if (logFile) { sdLoggingEnabled = true; Serial.println("[SD] Logging STARTED."); }
      else         { Serial.println("[SD] Could not open log file. Logging NOT started."); }
    } else {
      sdLoggingEnabled = false;
      closeLogFile();
      lastWriteOK = false;
      Serial.println("[SD] Logging STOPPED. File closed; safe to eject card.");
    }
    needOLEDRefresh = true;
  } while (false);

  server.handleClient();

  unsigned long now = millis();

  // SD health check: runs whenever card is thought to be present (every 2s)
  if (sdPresent && (now - lastSdHealthMs >= SD_HEALTH_INTERVAL_MS)) {
    lastSdHealthMs = now;
    bool lost = false;
    if (SD.cardType() == CARD_NONE) lost = true;
    else { File root = SD.open("/"); if (!root) lost = true; else root.close(); }
    if (lost) {
      sdPresent = false; sdOK = false;
      if (sdLoggingEnabled) {
        Serial.println("[SD] Card lost during logging — stopping now.");
        sdLoggingEnabled = false; closeLogFile(); lastWriteOK = false;
      } else {
        Serial.println("[SD] Card removed.");
      }
      needOLEDRefresh = true;
    }
  }

  // Sensors + OLED once/sec
  static unsigned long lastSensors = 0;
  if (now - lastSensors >= 1000) {
    lastSensors = now;
    t1 = readTCFiltered(tc1);
    t2 = readTCFiltered(tc2);
    t3 = readTCFiltered(tc3);
    drawOLED();
  }

  if (needOLEDRefresh) { needOLEDRefresh = false; drawOLED(); }

  // CSV logging once/min (only if enabled and file open)
  if (sdLoggingEnabled && (now - lastLogMs >= LOG_INTERVAL_MS)) {
    lastLogMs = now;
    logLine();      // auto-stop on write failure
    drawOLED();
  }
}
