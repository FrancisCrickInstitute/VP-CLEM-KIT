// UV_Cryo_V5_21 — Milestone 5.21
// ESP32 WROOM (Arduino-ESP32 v3.x)
//
// What’s in here:
// - I2C @ 100 kHz (SDA=23, SCL=22) driving OLED (0x3C) + VL53L0X (0x29)
// - OLED info screen only (no splash), updates ≥ 1 Hz
// - PWM on IO32 using LEDC @ 120 Hz; encoder adjusts duty immediately
// - Cooler MOSFET on IO33 via MCP1416 gate driver; turned ON after boot
// - Door interlock: if distance >= 1000 mm -> LED forced OFF (duty=0), restored below threshold
// - Encoder A/B on IO35/IO34 (interrupt-driven quadrature), Button on IO14 (interrupt), toggles SD logging
// - SPI (SCK=5, MISO=19, MOSI=18), CS: TC1=16, TC2=17, TC3=2, SD=4
// - Three MAX31855 thermocouples (hardware SPI)
// - Web GUI with simple plots; phone time sync; mDNS: http://cryoUV.local
// - SD logging: CSV once/min; “R” badge while actively logging
//   * Start logging: fully re-inits SPI and re-mounts SD (handles hot re-insert)
//   * While logging: every 2 s check SD presence (cardType + open("/")); stop immediately if lost
//   * Safe to eject after you stop logging
//
// Libraries: Adafruit_GFX, Adafruit_SSD1306, Adafruit_VL53L0X, Adafruit_MAX31855, SD, WiFi, WebServer, ESPmDNS

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
#include <Adafruit_VL53L0X.h>
#include <Adafruit_MAX31855.h>

// ---------------- Pins ----------------
const int LED_PWM_PIN = 32;
const int I2C_SDA = 23, I2C_SCL = 22;
const int SPI_SCK = 5, SPI_MISO = 19, SPI_MOSI = 18;
const int TC_CS1 = 16, TC_CS2 = 17, TC_CS3 = 2, SD_CS = 4;
const int ENC_A = 35, ENC_B = 34, ENC_BTN = 14;
const int COOLER_PIN = 33;
const int VL53_XSHUT_PIN = -1;  // -1 if tied HIGH

// ---------------- I2C ----------------
const uint32_t I2C_CLOCK_HZ = 100000;   // 100 kHz
const uint32_t I2C_TIMEOUT_MS = 20;

// ---------------- OLED ----------------
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64
Adafruit_SSD1306 display(SCREEN_WIDTH, SCREEN_HEIGHT, &Wire, -1);

// ---------------- VL53L0X ----------------
Adafruit_VL53L0X lox;
bool hasVL53 = false;
const int32_t VL53_MAX_MM = 1000;       // cap for out-of-bounds reports
const int32_t DOOR_OPEN_MM = 1000;      // interlock threshold

// ---------------- MAX31855 ----------------
Adafruit_MAX31855 tc1(TC_CS1), tc2(TC_CS2), tc3(TC_CS3);

// ---------------- PWM (LEDC) ----------------
const uint32_t LEDC_FREQ = 120;
const uint8_t  LEDC_RES_BITS = 10;

int intendedDuty = 30;                  // 0..100 set by encoder
volatile int dutyPercent = 30;          // applied (0 when door open)
bool ledEnabled = true;
const int DUTY_STEP = 5;

// Door state
bool doorOpen = false;

// ---------------- Encoder (interrupts) ----------------
volatile int32_t encDelta = 0;
volatile uint8_t encState = 0;
const int ENC_STEPS_PER_DETENT = 4;

// Button (interrupt)
volatile bool btnPressFlag = false;
unsigned long lastBtnHandledMs = 0;
const unsigned long BTN_DEBOUNCE_MS = 50;

// ---------------- WiFi / Web ----------------
const char* ssid = "cryoUV";
const char* password = "Photon2025!";
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
int32_t dist = -1;

// ---------------- SD / Logging ----------------
File logFile;
bool sdOK = false;            // last SD.begin() result
bool sdPresent = false;       // open("/") success
bool sdLoggingEnabled = true; // user intent
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
void handleEncoderDelta();
void IRAM_ATTR isrEncA();
void IRAM_ATTR isrEncB();
void IRAM_ATTR isrBtn();
void drawOLED();
void drawBadgeR();
bool initVL53();
int32_t readDistanceMM();
void updateDoorState();
bool sdReinitBusAndMount(uint32_t hz = 8000000);
bool sdTryInit();
void ensureLogOpen();
void closeLogFile();
void logLine();
String makeFilename();
void testWriteOnce();

void handleRoot();
void handleData();
void handleTime();
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

/** Read one MAX31855 with small retry and basic sanity filtering; return NAN on failure. */
double readTCFiltered(Adafruit_MAX31855 &tc) {
  ensureAllCSHigh();
  for (int attempt = 0; attempt < 3; ++attempt) {
    double c = tc.readCelsius();
    int16_t err = tc.readError();
    if (err == 0 && !isnan(c) && c > -200 && c < 1350) return c;
    delay(5);
  }
  return NAN;
}

// =================== PWM / Controls ===================

/** Write the current dutyPercent (0..100) to LEDC, respecting ledEnabled. */
void applyPWMDuty() {
  uint32_t raw = map(dutyPercent, 0, 100, 0, (1u << LEDC_RES_BITS) - 1);
  ledcWrite(LED_PWM_PIN, ledEnabled ? raw : 0);
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

/** Consume quadrature steps from ISR and adjust intendedDuty in detent-sized steps. */
void handleEncoderDelta() {
  static int32_t acc = 0;
  int32_t d = encDelta;
  if (d != 0) {
    encDelta = 0;
    acc += d;
    while (acc >= ENC_STEPS_PER_DETENT) { intendedDuty = min(100, intendedDuty + DUTY_STEP); acc -= ENC_STEPS_PER_DETENT; }
    while (acc <= -ENC_STEPS_PER_DETENT){ intendedDuty = max(0,   intendedDuty - DUTY_STEP); acc += ENC_STEPS_PER_DETENT; }
    updateEffectiveDutyFromDoor();
  }
}

// ---- Encoder ISRs ----
static const int8_t ENC_DIR[16] = {
  0, -1, +1,  0,
 +1,  0,  0, -1,
 -1,  0,  0, +1,
  0, +1, -1,  0
};

/** Common ISR for A/B: decode Gray transitions into +/-1 steps. */
void IRAM_ATTR isrEncCommon() {
  uint8_t a = (uint8_t)digitalRead(ENC_A);
  uint8_t b = (uint8_t)digitalRead(ENC_B);
  uint8_t curr = (a ? 2 : 0) | (b ? 1 : 0);
  uint8_t idx = ((encState & 0x03) << 2) | curr;
  encDelta += ENC_DIR[idx & 0x0F];
  encState = curr;
}
void IRAM_ATTR isrEncA() { isrEncCommon(); }
void IRAM_ATTR isrEncB() { isrEncCommon(); }

/** ISR for the button: set a flag; loop will debounce and act. */
void IRAM_ATTR isrBtn() {
  btnPressFlag = true;
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

// =================== OLED ===================

/** Draw a white circular "R" badge at top-right while actively logging (file open). */
void drawBadgeR() {
  bool loggingActive = sdLoggingEnabled && logFile;
  if (!loggingActive) return;
  int r = 7, cx = SCREEN_WIDTH - r - 2, cy = r + 2;
#ifdef Adafruit_GFX_h
  display.fillCircle(cx, cy, r, SSD1306_WHITE);
#else
  display.fillRect(cx - r, cy - r, 2*r, 2*r, SSD1306_WHITE);
#endif
  display.setTextColor(SSD1306_BLACK);
  display.setCursor(cx - 3, cy - 4);
  display.print("R");
  display.setTextColor(SSD1306_WHITE);
}

/** Render the full OLED screen (LED duty, 3 temps, distance + [Open]/[Close], timestamp, badge). */
void drawOLED() {
  if (!i2cPing(0x3C)) return;
  display.clearDisplay();
  display.setTextSize(1);
  display.setTextColor(SSD1306_WHITE);

  int y = 0;
  display.setCursor(0, y); display.println("UV Control"); y += 8;

  display.setCursor(0, y);
  display.print("LED: "); display.print(dutyPercent); display.println("%"); y += 8;

  display.setCursor(0, y); display.print("T1: ");
  if (isnan(t1)) display.println("N/A"); else { display.print(t1, 1); display.println(" C"); } y += 8;

  display.setCursor(0, y); display.print("T2: ");
  if (isnan(t2)) display.println("N/A"); else { display.print(t2, 1); display.println(" C"); } y += 8;

  display.setCursor(0, y); display.print("T3: ");
  if (isnan(t3)) display.println("N/A"); else { display.print(t3, 1); display.println(" C"); } y += 8;

  display.setCursor(0, y); display.print("Dist: ");
  if (dist >= 0) {
    int32_t shown = min(dist, VL53_MAX_MM);
    display.print(shown); display.print(" mm ");
    display.print(doorOpen ? "[Open]" : "[Close]");
  } else {
    display.print("N/A  [?]");
  }

  String ts = isoNow();
  int16_t x1, y1; uint16_t w, h;
  display.getTextBounds(ts, 0, 0, &x1, &y1, &w, &h);
  int baseY = SCREEN_HEIGHT - (int)h - 1; if (baseY < 0) baseY = 0;
  display.setCursor(0, baseY);
  display.println(ts);

  drawBadgeR();
  display.display();
}

// =================== VL53L0X ===================

/** Reset (via XSHUT if available), probe 0x29, begin(), and report; returns true on success. */
bool initVL53() {
  if (VL53_XSHUT_PIN >= 0) { pinMode(VL53_XSHUT_PIN, OUTPUT); digitalWrite(VL53_XSHUT_PIN, LOW); delay(10); digitalWrite(VL53_XSHUT_PIN, HIGH); delay(20); }
  else delay(20);
  if (!i2cPing(0x29)) { Serial.println("[VL53] 0x29 not ACKing; check XSHUT/VCC/SDA/SCL."); return false; }
  if (!lox.begin(0x29)) { Serial.println("[VL53] begin(0x29) failed (device ACKed)."); return false; }
  Serial.println("[VL53] VL53L0X init OK at 0x29.");
  return true;
}

/** Read one measurement; RangeStatus==0 -> distance; ==4 -> capped to VL53_MAX_MM; else -1. */
int32_t readDistanceMM() {
  if (!hasVL53) return -1;
  VL53L0X_RangingMeasurementData_t m;
  lox.rangingTest(&m, false);
  if (m.RangeStatus == 0) return (int32_t)m.RangeMilliMeter;
  if (m.RangeStatus == 4) return VL53_MAX_MM;
  return -1;
}

/** Update doorOpen from current dist and enforce PWM if the state toggled. */
void updateDoorState() {
  bool prev = doorOpen;
  if (dist >= 0 && dist >= DOOR_OPEN_MM) doorOpen = true;
  else if (dist >= 0 && dist < DOOR_OPEN_MM) doorOpen = false;
  if (doorOpen != prev) {
    Serial.println(doorOpen ? "[DOOR] OPEN — forcing LEDs OFF." : "[DOOR] CLOSED — restoring PWM.");
    updateEffectiveDutyFromDoor();
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
  for (int i = 0; i < 10000; i++) {
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
    logFile.println("timestamp,pwm,enabled,T1_C,T2_C,T3_C,Dist_mm,Door");
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
  String line = ts + "," + String(dutyPercent) + "," + (ledEnabled ? "1" : "0") + ",";
  line += (isnan(t1) ? String("") : String(t1, 2)); line += ",";
  line += (isnan(t2) ? String("") : String(t2, 2)); line += ",";
  line += (isnan(t3) ? String("") : String(t3, 2)); line += ",";
  line += (dist < 0 ? String("") : String(min(dist, VL53_MAX_MM))); line += ",";
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

/** One-time boot test: write a single BOOT line (does not keep file open). */
void testWriteOnce() {
  if (!sdOK || !sdPresent) return;
  String fname = makeFilename();
  File f = SD.open(fname, FILE_WRITE);
  if (f) {
    String line = isoNow() + ",BOOT,,,,,,"; // matches header column count
    f.println(line);
    f.flush();
    f.close();
    Serial.print("[SD] Wrote boot test line to "); Serial.println(fname);
  } else {
    Serial.println("[SD] Boot test write: failed to open file.");
  }
}

// =================== Web / Time ===================

/** Serve minimal dashboard with plots + “Sync time” button. */
void handleRoot() {
  String html = R"HTML(
<!DOCTYPE html><html><head><meta charset="utf-8">
<title>cryoUV Monitor</title>
<style>
 body{font-family:system-ui,Arial,sans-serif;margin:16px}
 .row{display:flex;gap:16px;flex-wrap:wrap}
 .card{border:1px solid #ccc;border-radius:8px;padding:12px;min-width:280px}
 canvas{width:320px;height:160px;border:1px solid #ddd;border-radius:4px}
 .big{font-size:18px;margin:6px 0}
 button{padding:6px 10px;border-radius:6px;border:1px solid #aaa;background:#f5f5f5}
</style>
</head><body>
<h2>cryoUV Monitor</h2>
<p id="summary" class="big">Loading…</p>
<p>Time: <span id="time">—</span> <button onclick="sendTime()">Sync time</button></p>

<div class="row">
  <div class="card"><h3>PWM (%)</h3><canvas id="pwm"></canvas></div>
  <div class="card"><h3>Distance (mm)</h3><canvas id="dist"></canvas></div>
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
    ctx.strokeStyle='#0066cc'; ctx.beginPath(); let first=true;
    for(let i=0;i<this.y.length;i++){
      const x=left + (i/(this.maxN-1))*W; const v=this.y[i]; if(isNaN(v)) { first=true; continue; }
      const y=top + (1-((v-this.ymin)/(this.ymax-this.ymin)))*H;
      if(first){ ctx.moveTo(x,y); first=false; } else ctx.lineTo(x,y);
    } ctx.stroke();
  }
}
const plots={ pwm:new MiniPlot('pwm',0,100), dist:new MiniPlot('dist',0,2000),
  t1:new MiniPlot('t1',-20,50), t2:new MiniPlot('t2',-20,50), t3:new MiniPlot('t3',-20,50) };
async function tick(){
  try{
    const r=await fetch('/data'); const j=await r.json();
    const door = j.door ? "Door Open" : "Door Closed";
    document.getElementById('summary').innerText =
      `PWM: ${j.pwm}% ${j.enabled?'[ON]':'[OFF]'} | T1: ${j.t1??'—'}°C  T2: ${j.t2??'—'}°C  T3: ${j.t3??'—'}°C  | Dist: ${j.dist??'—'} mm [${door}]`;
    document.getElementById('time').innerText = j.time ?? '—';
    plots.pwm.push(j.pwm); plots.dist.push(j.dist); plots.t1.push(j.t1); plots.t2.push(j.t2); plots.t3.push(j.t3);
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
  json += "\"enabled\":" + (String)(ledEnabled ? "true" : "false") + ",";
  json += "\"t1\":" + (isnan(t1) ? String("null") : String(t1, 1)) + ",";
  json += "\"t2\":" + (isnan(t2) ? String("null") : String(t2, 1)) + ",";
  json += "\"t3\":" + (isnan(t3) ? String("null") : String(t3, 1)) + ",";
  json += "\"dist\":" + (dist < 0 ? String("null") : String(min(dist, VL53_MAX_MM))) + ",";
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

/** Initialize all subsystems (I²C, OLED, SPI/SD try, VL53, PWM, encoder/button ISRs, Wi-Fi/web) and draw once. */
void setup() {
  Serial.begin(115200);
  delay(50);

  i2cBeginOnCustomPins();
  delay(30);

  if (i2cPing(0x3C)) {
    if (!display.begin(SSD1306_SWITCHCAPVCC, 0x3C)) Serial.println("OLED begin failed (0x3C ACKed).");
  } else {
    Serial.println("OLED not found at 0x3C.");
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
  if (sdPresent) testWriteOnce();

  // VL53L0X
  hasVL53 = initVL53();

  // PWM
  ledcAttach(LED_PWM_PIN, LEDC_FREQ, LEDC_RES_BITS);
  dutyPercent = intendedDuty; applyPWMDuty();

  // Encoder ISRs
  pinMode(ENC_A, INPUT); pinMode(ENC_B, INPUT);  // input-only pins; use external pull-ups
  encState = ((digitalRead(ENC_A) ? 2 : 0) | (digitalRead(ENC_B) ? 1 : 0));
  attachInterrupt(digitalPinToInterrupt(ENC_A), isrEncA, CHANGE);
  attachInterrupt(digitalPinToInterrupt(ENC_B), isrEncB, CHANGE);

  // Button ISR
  pinMode(ENC_BTN, INPUT_PULLUP);
  attachInterrupt(digitalPinToInterrupt(ENC_BTN), isrBtn, FALLING);

  // WiFi AP + mDNS + web
  WiFi.mode(WIFI_AP);
  WiFi.softAP(ssid, password);
  IPAddress ip = WiFi.softAPIP();
  Serial.print("[AP] IP: "); Serial.println(ip);
  if (MDNS.begin("cryoUV")) { MDNS.addService("http", "tcp", 80); Serial.println("[mDNS] Hostname: http://cryoUV.local"); }
  server.on("/", handleRoot);
  server.on("/data", handleData);
  server.on("/time", HTTP_POST, handleTime);
  server.begin();

  // Cooler ON after all subsystems initialised
  pinMode(COOLER_PIN, OUTPUT);
  digitalWrite(COOLER_PIN, HIGH);
  Serial.println("[COOLER] Cooler ON (GPIO33 HIGH).");

  // First reads + draw
  t1 = readTCFiltered(tc1); t2 = readTCFiltered(tc2); t3 = readTCFiltered(tc3);
  dist = readDistanceMM();
  updateDoorState();
  drawOLED();
}

/** Main loop: handle encoder/button, HTTP, SD health (if logging), sensors+OLED each second, CSV once/min. */
void loop() {
  handleEncoderDelta();

  // Button press: on-demand SD init when starting logging
  if (btnPressFlag) {
    btnPressFlag = false;
    unsigned long now = millis();
    if (now - lastBtnHandledMs >= BTN_DEBOUNCE_MS) {
      lastBtnHandledMs = now;

      if (!sdLoggingEnabled) {
        // Always reinit bus + mount on each start to handle hot re-insert
        if (!sdTryInit()) {
          Serial.println("[SD] Cannot START logging (no SD / RO / init failed).");
          needOLEDRefresh = true;
          goto after_button;
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
    }
  }
after_button:

  server.handleClient();

  unsigned long now = millis();

  // Fast SD health check while logging (every 2s)
  if (sdLoggingEnabled && logFile && (now - lastSdHealthMs >= SD_HEALTH_INTERVAL_MS)) {
    lastSdHealthMs = now;
    bool lost = false;
    if (SD.cardType() == CARD_NONE) lost = true;
    else { File root = SD.open("/"); if (!root) lost = true; else root.close(); }
    if (lost) {
      Serial.println("[SD] Card lost during logging — stopping now.");
      sdLoggingEnabled = false; closeLogFile(); lastWriteOK = false; needOLEDRefresh = true;
    }
  }

  // Sensors + OLED once/sec
  static unsigned long lastSensors = 0;
  if (now - lastSensors >= 1000) {
    lastSensors = now;
    t1 = readTCFiltered(tc1);
    t2 = readTCFiltered(tc2);
    t3 = readTCFiltered(tc3);
    dist = readDistanceMM();
    updateDoorState();
    drawOLED();
  }

  if (needOLEDRefresh) { needOLEDRefresh = false; drawOLED(); }

  // CSV logging once/min (only if enabled and file open)
  if (sdLoggingEnabled && (now - lastLogMs >= LOG_INTERVAL_MS)) {
    lastLogMs = now;
    logLine();      // auto-stop on write failure
    drawOLED();     // keep 'R' badge accurate
  }
}
