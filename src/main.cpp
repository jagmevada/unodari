#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#include <WiFiManager.h>
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <RTClib.h>
//  #define FIRSTTIME // <-- Uncomment for first RTC setup, then comment out after RTC is set



// =============================
// RTC/NTP Drift Correction State
// =============================
#define RTC_NTP_DRIFT_CHECK_INTERVAL_MS 600000UL // 10 minutes
#define RTC_NTP_DRIFT_THRESHOLD_SEC 120 // 2 minutes
unsigned long lastRTCDriftCheck = 0;

// =============================
// Time Source State
// =============================
enum TimeSource {
  TIME_NONE,
  TIME_NTP,
  TIME_RTC
};
TimeSource g_timeSource = TIME_NONE;
bool g_timeValid = false;
char g_timeErrorMsg[48] = "";


/*

  Project overview for Copilot (ESP32 + TCRT + Keypad + OLED UI):

  ==========================================
  New Feature (Dec 2025): DS3231 RTC Fallback & Robust Time Handling
  ==========================================
  - Adds DS3231 RTC on a separate I2C bus (GPIO25=RTC_SDA, GPIO26=RTC_SCL, 100kHz) using RTClib.
  - On boot, attempts NTP sync (non-blocking, with timeout). If NTP fails, uses RTC for timekeeping.
  - If neither NTP nor RTC is valid, system pauses token counting and backend sync, and displays error.
  - If NTP later becomes available, compares NTP and RTC; if drift >2min, updates RTC from NTP.
  - RTC always stores UTC; IST offset is applied for display and meal logic.
  - All time source decisions and RTC updates are logged to Serial and displayed on OLED if error.
  - See code for details and future extensibility.

  ==========================================
  Hardware summary
  ==========================================

  - MCU: ESP32
  - Input devices:
      1) 4x push buttons used as a simple keypad:
           KEY1_PIN = 5   // Button "1"
           KEY2_PIN = 17  // Button "2"
           KEY3_PIN = 19  // Button "3"
           KEY4_PIN = 18  // Button "4"
         - Buttons are wired as active-LOW with INPUT_PULLUP.
         - External interrupts on FALLING edge.
         - ISRs only set flags; actual handling + debounce is done in loop().

      2) 2x TCRT5000 reflective IR sensors:
           Sensor 1:
             Digital output -> S1_D0_PIN = 23
             Analog output  -> S1_A0_PIN = 32
           Sensor 2:
             Digital output  -> S2_D0_PIN = 16
             Analog output   -> S2_A0_PIN = 33
         - Analog is read using 12-bit ADC (0..4095) via analogReadResolution(12).

         IMPORTANT (current token counter implementation):
         - Token detection uses ONLY the analog inputs (S1_A0_PIN, S2_A0_PIN).
         - Digital inputs (S1_D0_PIN, S2_D0_PIN) are still read for debug but
           are NOT used for counting.

  - Output device:
      - 128x64 SH1106 OLED over I2C
        Using U8g2 full-buffer driver:
          U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
        Default ESP32 I2C pins: SDA = 21, SCL = 22.

  ==========================================
  UI layout (OLED)
  ==========================================

  Full screen: 128x64 pixels.

  Top row:
    - Left side: header text "D" in big font (u8g2_font_10x20_tf).
    - Right side: status icons + time:
        [ Time "hh:mm AM/PM" ] [ WiFi icon ] [ Battery icon ]

  WiFi icon (left of battery):
    - Drawn by drawWifi(uint8_t levelIndex).
    - Position: top-right area, left of battery, 12x10 approx.
    - Semantic:
        levelIndex == 0 → circle with slash "no network" symbol.
        levelIndex  1-4 → vertical bars like 4G/cellular signal:
                          1,2,3,4 bars visible (increasing height).

  Battery icon (rightmost):
    - Drawn by drawBattery(uint8_t levelIndex).
    - Position: top-right corner.
    - Rendering:
        - Battery outline + small terminal on the right.
        - 5 possible fill bars inside (0..4).
    - Semantic mapping:
        levelIndex 0..4 → 0%, 25%, 50%, 75%, 100% battery.

  Middle / bottom area (original design):
    - Sensor/diagnostic text in smaller bold font (u8g2_font_8x13B_tf):
        Line 1: "S1 A:<analog> D:<H/L>"
        Line 2: "S2 A:<analog> D:<H/L>"
        Line 3: "Last key: <string>"

  NOTE (current version – token counter UI):
    - The middle/bottom area is now used to display a large **token count**
      (0..9999) using u8g2_font_logisoso32_tf.
    - Sensor values and last key information are still kept in code and printed
      to Serial for diagnostics, but not shown on OLED in this revision.

  ==========================================
  Software structure (high level)
  ==========================================

  - setup():
      * setupSerial()
      * setupDisplay()
      * setupSensors()
      * setupKeypad()

  - loop():
      * readSensors()
          - Reads analog + digital values for both TCRT5000 sensors.
          - Runs analog-based Schmitt-trigger logic on each 1ms sample to
            detect fast dips.
          - Applies OR logic between sensors within a small time window
            to decide if it is one token or two.
          - Updates g_tokenCount accordingly.
          - Prints structured values to Serial for use with
            the VS Code Serial Plotter plugin (Mario Zechner style).
      * handleKeypad()
          - processKey() pattern:
              - Checks interrupt flags (set by ISRs).
              - Does software debouncing with BUTTON_DEBOUNCE_MS and millis().
              - Confirms button is still LOW before calling handler.
          - Handlers onKey1Pressed()..onKey4Pressed() update token counter
            and bundle mode (Key1 reset, Key2 +10-bundle, Key3/4 reserved).
      * updateDisplay()
          - Clears U8g2 buffer, calls drawScreen(), then sendBuffer().

  - drawScreen():
      * Draws header "D".
      * Draws time string, WiFi icon, battery icon on the same top row.
      * Draws the token count (0..9999) in a large, centered font.

  ==========================================
  UI state variables for backend integration
  ==========================================

  These are the key globals that backend / WiFi / battery / time / counter code
  should update. Copilot should use these when integrating real logic:

    - uint8_t g_batteryLevelIndex
        Range: 0..4
        Meaning:
          0 → 0% / empty
          1 → ~25%
          2 → ~50%
          3 → ~75%
          4 → ~100%

    - uint8_t g_wifiLevelIndex
        Range: 0..4
        Meaning:
          0 → "no network" (circle with slash)
          1 → 1 bar (weak)
          2 → 2 bars
          3 → 3 bars
          4 → 4 bars (full / strong)

    - String g_timeString
        Meaning:
          - Formatted local time string, e.g. "07:45 PM".

    - int g_tokenCount
        Range: 0..9999
        Meaning:
          - Main token / coupon count shown in large font.
        Usage:
          - Automatically updated from analog IR events.
          - Backend / EEPROM can also set/restore it.

    - bool g_addBundle10
        Meaning:
          - If true, the next valid token event increments by +10 instead of +1.

  ==========================================
  Time display (top bar clock for backend)
  ==========================================

  - A global string:
        String g_timeString
        Example value: "12:45 PM"

  - drawScreen() prints this time string on the top row,
    to the left of the WiFi and Battery icons (status bar style).

  - Backend (NTP / RTC / WiFi time sync) should:
        * Format current time as "hh:mm AM/PM"
        * Assign it to g_timeString whenever time updates.

  ==========================================
  Token counter logic (ANALOG + Schmitt trigger + OR window)
  ==========================================

  - Sampling:
        - Analog inputs S1_A0_PIN and S2_A0_PIN are sampled every 1ms.
        - Sampling rate is controlled by IR_SAMPLE_INTERVAL_MS (macro).

  - Schmitt trigger thresholds (macros):
        - IR_LTH = 2200  (Low threshold)
        - IR_HTH = 2700  (High threshold)
        - The idea:
            * When a sensor is "idle" (HIGH side), its analog value is above IR_HTH.
            * When a fast object passes, the value briefly dips below IR_LTH.

        - Per-sensor state:
            * Start in HIGH region (idle).
            * If in HIGH region and analog <= IR_LTH → this is a local event,
              and state moves to LOW region.
            * If in LOW region and analog >= IR_HTH → re-arm back to HIGH region.
            * This gives hysteresis and noise immunity.

  - OR-ing between sensors with time window:
        - Macro TOKEN_MERGE_WINDOW_MS = 5ms.
        - If a sensor's Schmitt trigger reports a local event at time t,
          we compare t against the last global token event timestamp:
              if (t - lastTokenEventTime >= TOKEN_MERGE_WINDOW_MS)
                  → This is a **new** token → count up (+1 or +10).
                  → lastTokenEventTime = t.
              else
                  → This sensor event is considered the same physical token
                    (e.g., passing through both sensors nearly together),
                    so we ignore it and do not increment.

        - Effect:
            * If both sensors are triggered within 5ms → counted as **one token**.
            * If they are triggered outside the 5ms window → counted as
              **two separate tokens inserted**.

  - Keypad mapping:
        * Key 1:
            - Reset counter to zero (g_tokenCount = 0).
        * Key 2:
            - Enable bundle mode (+10 on next token event).
            - Sets g_addBundle10 = true (one-shot).
        * Key 3:
            - Reserved (only updates g_lastKeyPressed).
        * Key 4:
            - Reserved (only updates g_lastKeyPressed).

  ==========================================
  Notes for future Copilot assistance
  ==========================================

  - All IR counting logic is analog-based now; digital inputs are for optional
    diagnostics only.
  - If needed, IR_LTH / IR_HTH / IR_SAMPLE_INTERVAL_MS / TOKEN_MERGE_WINDOW_MS
    can be tuned for your mechanics and optics.
  - When integrating WiFi, battery, or time, only update the corresponding
    global variables; the drawing functions are pure "view".
*/



// =============================
// RTC DS3231 on separate I2C bus (GPIO25=RTC_SDA, GPIO26=RTC_SCL)
// =============================
#define RTC_SDA_PIN 25
#define RTC_SCL_PIN 26
TwoWire rtcWire = TwoWire(1); // Use I2C bus 1 for RTC
RTC_DS3231 rtc;
// =============================
// Pin Definitions
// =============================

// Keypad (4 buttons)
#define KEY1_PIN  5    // Button "1"
#define KEY2_PIN  17   // Button "2"
#define KEY3_PIN  19   // Button "3"
#define KEY4_PIN  18   // Button "4"

// TCRT5000 Sensor 1
#define S1_D0_PIN 23   // Digital (not used for counting)
#define S1_A0_PIN 32   // Analog (used for counting)

// TCRT5000 Sensor 2
#define S2_D0_PIN 16   // Digital (not used for counting)
#define S2_A0_PIN 33   // Analog (used for counting)

// I2C pins for ESP32 (OLED) - hardware default: SDA=21, SCL=22

// =============================
// Config Macros
// =============================

#define BUTTON_DEBOUNCE_MS     50UL   // debounce time for keypad (in ms)
#define LOOP_DELAY_MS          1UL    // main loop delay, keep small for fast sampling

// Analog sampling and Schmitt trigger thresholds for IR
#define IR_SAMPLE_INTERVAL_MS  1UL    // sample analog inputs every 1ms
#define IR_LTH                 2200   // low threshold for Schmitt trigger
#define IR_HTH                 2700   // high threshold for Schmitt trigger

// Time window for OR-ing between two sensors (ms)
#define TOKEN_MERGE_WINDOW_MS  110UL    // if events are closer than this, count as one token

// Meal window macros (IST)
#define BFL 0
#define BFH 9
#define LFL 11
#define LFH 14
#define DFL 18
#define DFH 21
#define DUMMYHREFORTESTING 0 // Set to 0 for production, >0 for testing

// Time sync config
#ifndef TIME_SYNC_INTERVAL_MS
#define TIME_SYNC_INTERVAL_MS 3600000UL // 1 hour
#endif
#ifndef TIME_RETRY_INTERVAL_MS
#define TIME_RETRY_INTERVAL_MS 600000UL // 10 minutes
#endif
#ifndef TIME_INITIAL_TIMEOUT_MS
#define TIME_INITIAL_TIMEOUT_MS 5000UL
#endif
#ifndef TIME_SYNC_ATTEMPT_TIMEOUT_MS
#define TIME_SYNC_ATTEMPT_TIMEOUT_MS 10000UL
#endif

#define TIME_SYNC_DATA_INTERVAL_MS 10000UL // 10 seconds

// India Standard Time offset from UTC in seconds (+5:30)
static const long IST_OFFSET_SECONDS = 5 * 3600 + 30 * 60;
volatile unsigned long now=0; // current time in loop
volatile unsigned long lastWiFiCheck = 0;
volatile unsigned long lastEEPROMWrite = 0;
volatile unsigned long lastSensorSend = 0;
// =============================
// Global State
// =============================

// Sensor readings (for debug / plotting)
int  g_sensor1Analog = 0;
int  g_sensor2Analog = 0;
bool g_sensor1Digital = false;
bool g_sensor2Digital = false;

// Schmitt trigger state for each sensor (true = high region, false = low region)
bool g_s1HighRegion = true;
bool g_s2HighRegion = true;

// Token counter 0..9999
int  g_tokenCount = 0;
// Bundle +10 flag (Key2 sets, then next token event uses +10 and clears it)
bool g_addBundle10 = false;

// Track previous token count for immediate HTTP send
int g_tokenCountPrevious = 0;

// Last time we sampled the analog IR (for 1ms sampling)
uint32_t g_lastIrSampleMs = 0;

// Last time any token event was counted (for OR-window between sensors)
uint32_t g_lastTokenEventMs = 0;

// Last pressed key info (for debug/Serial/logging)
String g_lastKeyPressed = "None";

// Interrupt flags for buttons (set in ISR, handled in loop)
volatile bool g_key1Interrupt = false;
volatile bool g_key2Interrupt = false;
volatile bool g_key3Interrupt = false;
volatile bool g_key4Interrupt = false;

// Debounce timestamps for keypad
uint32_t g_key1LastPressMs = 0;
uint32_t g_key2LastPressMs = 0;
uint32_t g_key3LastPressMs = 0;
uint32_t g_key4LastPressMs = 0;

// Battery level: 0..4 => 0,25,50,75,100%
uint8_t g_batteryLevelIndex = 4;  // TODO: backend should update based on real battery

// WiFi level/state: 0..4
//  0 -> no-network symbol (circle with slash)
//  1..4 -> that many bars
uint8_t g_wifiLevelIndex = 4;     // TODO: backend should update based on WiFi status/RSSI

// Time string in "hh:mm AM/PM" format (to be updated by backend NTP/RTC code)
String g_timeString = "12:00 AM"; // TODO: backend should set real time here

// Supabase backend config
const char *deviceId = "uno_1"; // Change to uno_2 or uno_3 for other devices
const char *deviceId2 = "uno_2";
const char *deviceId3 = "uno_3";
const char *postURL = "https://akxcjabakrvfaevdfwru.supabase.co/rest/v1/unodari_token";
const char *apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFreGNqYWJha3J2ZmFldmRmd3J1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkxMjMwMjUsImV4cCI6MjA2NDY5OTAyNX0.kykki4uVVgkSVU4lH-wcuGRdyu2xJ1CQkYFhQq_u08w";

enum mealType {
  NONE,
  BREAKFAST,
  LUNCH,
  DINNER,
  MEAL_COUNT
};
String meal[MEAL_COUNT] = {"none", "breakfast", "lunch", "dinner"};
mealType currentMeal = NONE;



struct TokenData {
  int token_count;
  mealType meal;
  char date[11];    // "yyyy-mm-dd"
  bool update;
};

TokenData token_data = {0, NONE, "1970-01-01", false};
TokenData token_data2 = {0, NONE, "1970-01-01", false};
TokenData token_data3 = {0, NONE, "1970-01-01", false};
Preferences prefs;
// =============================
// HTTP send queue + background task
// =============================

struct SendJob {
  const char *deviceId;  // "uno_1", "uno_2", "uno_3"
  TokenData   data;      // copy of token_data (small struct)
};

// Queue handle for pending HTTP jobs
QueueHandle_t g_sendQueue = nullptr;


// =============================
// Forward Declarations (all functions defined later)
// =============================
void httpSenderTask(void *pvParameters);
void sendTokenData(const char *id, const TokenData *token_data);
void checkWiFi();
void setupSerial();
void setupDisplay();
void setupSensors();
void setupKeypad();
void readSensors();
void handleKeypad();
void updateDisplay();
void drawScreen();
void drawBattery(uint8_t levelIndex);
void drawWifi(uint8_t levelIndex);
void processKey(uint8_t pin, volatile bool &interruptFlag, uint32_t &lastPressMs, void (*handler)());
void onKey1Pressed();
void onKey2Pressed();
void onKey3Pressed();
void onKey4Pressed();
void IRAM_ATTR isrKey1();
void IRAM_ATTR isrKey2();
void IRAM_ATTR isrKey3();
void IRAM_ATTR isrKey4();
inline bool isRTCValid(const DateTime& dt);
inline void setSystemTimeFromRTC(const DateTime& dt);
inline void setRTCFromSystemTime();
inline void logCurrentISTTime();
void setRTCFromNTP();



// fetchNetworkTime must be defined before TimeManager uses it
time_t fetchNetworkTime(unsigned long timeoutMs = 5000) {
  time_t now = time(nullptr);
  const time_t validThreshold = 1000000000;
  unsigned long start = millis();
  while (now < validThreshold && (millis() - start) < timeoutMs) {
    delay(50);
    now = time(nullptr);
  }
  if (now < validThreshold) return (time_t)-1;
  return now;
}

// TimeManager for NTP sync
class TimeManager {
public:
  TimeManager()
      : lastSyncedEpoch(0), lastSyncMillis(0), lastAttemptMillis(0),
        syncIntervalMs(TIME_SYNC_INTERVAL_MS), retryIntervalMs(TIME_RETRY_INTERVAL_MS),
        lastSyncSuccess(false), lastNoUpdateLogMillis(0) {}

  void begin(unsigned long intervalMs = TIME_SYNC_INTERVAL_MS, unsigned long initialTimeoutMs = TIME_INITIAL_TIMEOUT_MS) {
    syncIntervalMs = intervalMs;
    lastAttemptMillis = millis();
    if (!trySync(initialTimeoutMs)) {
      lastSyncedEpoch = time(nullptr);
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      Serial.println("⚠️ TimeManager: Initial NTP sync failed — continuing with local clock");
    }
  }

  bool trySync(unsigned long timeoutMs = TIME_SYNC_ATTEMPT_TIMEOUT_MS) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ TimeManager: NTP sync skipped — WiFi not connected");
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      return false;
    }
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    unsigned long attemptStart = millis();
    time_t epoch = fetchNetworkTime(timeoutMs);
    unsigned long took = millis() - attemptStart;
    if (epoch == (time_t)-1) {
      Serial.printf("⚠️ TimeManager: NTP attempt timed out after %lums (time() still %ld)\n", took, (long)time(nullptr));
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      return false;
    }
    Serial.printf("⏱ TimeManager: NTP returned epoch %ld after %lums\n", (long)epoch, took);
    if (epoch >= validThreshold) {
      time_t expected = lastSyncedEpoch + (time_t)((attemptStart - lastSyncMillis) / 1000);
      long diff = (long)epoch - (long)expected;
      if (lastSyncMillis == 0 || llabs(diff) > 2) {
        lastSyncedEpoch = epoch;
        lastSyncMillis = millis();
        lastSyncSuccess = true;
        struct tm tinfo;
        gmtime_r(&epoch, &tinfo);
        char buf[32];
        strftime(buf, sizeof(buf), "%FT%TZ", &tinfo);
        Serial.printf("⏱ TimeManager: NTP sync succeeded: %s\n", buf);
        return true;
      } else {
        lastSyncSuccess = true;
        lastSyncMillis = millis();
        if ((millis() - lastNoUpdateLogMillis) >= retryIntervalMs) {
          Serial.println("ℹ️ TimeManager: Time check OK — no new NTP update (using local clock)");
          lastNoUpdateLogMillis = millis();
        }
        return false;
      }
    }
    Serial.printf("⚠️ TimeManager: NTP returned invalid epoch: %ld\n", (long)epoch);
    lastSyncSuccess = false;
    lastAttemptMillis = millis();
    return false;
  }

  void update() {
    unsigned long nowMs = millis();
    unsigned long effectiveInterval = lastSyncSuccess ? syncIntervalMs : retryIntervalMs;
    if (lastSyncSuccess) {
      if ((nowMs - lastSyncMillis) >= effectiveInterval) {
        trySync(TIME_SYNC_ATTEMPT_TIMEOUT_MS);
      }
    } else {
      if ((nowMs - lastAttemptMillis) >= effectiveInterval) {
        trySync(TIME_SYNC_ATTEMPT_TIMEOUT_MS);
      }
    }
  }

  time_t now() const {
    unsigned long elapsedMs = millis() - lastSyncMillis;
    return lastSyncedEpoch + (time_t)(elapsedMs / 1000);
  }

  void getUTCTime(struct tm &out) const {
    time_t t = now();
    gmtime_r(&t, &out);
  }

private:
  time_t lastSyncedEpoch;
  unsigned long lastSyncMillis;
  unsigned long syncIntervalMs;
  unsigned long retryIntervalMs;
  bool lastSyncSuccess;
  unsigned long lastAttemptMillis;
  unsigned long lastNoUpdateLogMillis;
  static const time_t validThreshold = 1000000000;
};

TimeManager timeManager;
void httpSenderTask(void *pvParameters) ;
void sendTokenData(const char *id, const TokenData *token_data) ;
void sendTokenData(const char *id, const TokenData *token_data) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;

  // Optional: shorter timeout so even the HTTP task doesn't block forever
  http.setTimeout(1000);  // 1s timeout instead of long default

  String url = String(postURL) + "?sensor_id=eq." + id + "&date=eq." + token_data->date;
  http.begin(url);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));
  http.addHeader("Prefer", "return=representation");

  StaticJsonDocument<256> doc;
  doc[meal[token_data->meal]] = token_data->token_count;
  String payload;
  serializeJson(doc, payload);

  int code = http.sendRequest("PATCH", payload);  // blocking, but only in HTTP task now
  http.end();

  Serial.printf("HTTP send (%s) -> code %d\n", id, code);
}

void httpSenderTask(void *pvParameters) {
  SendJob job;
  for (;;) {
    // Block here until there is a job
    if (xQueueReceive(g_sendQueue, &job, portMAX_DELAY) == pdTRUE) {
      // Do the blocking HTTP call in this background task
      sendTokenData(job.deviceId, &job.data);
    }
    // loop again and wait for the next job
  }
}


void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    WiFi.disconnect();
    WiFi.begin();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
    }
    if (WiFi.status() == WL_CONNECTED) {
      return;
    }
    WiFiManager wm;
    wm.setConfigPortalTimeout(120);
    String setupName = String(deviceId) + "_SETUP";
    if (!wm.autoConnect(setupName.c_str())) {
      delay(1000);
      ESP.restart();
    }
  }
}

// =============================
// OLED Display (U8g2)
// =============================

U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);

// =============================
// Forward Declarations
// =============================

void setupSerial();
void setupDisplay();
void setupSensors();
void setupKeypad();

void readSensors();
void handleKeypad();
void updateDisplay();

void onKey1Pressed();
void onKey2Pressed();
void onKey3Pressed();
void onKey4Pressed();

void drawScreen();
void drawBattery(uint8_t levelIndex);
void drawWifi(uint8_t levelIndex);

// ISRs
void IRAM_ATTR isrKey1();
void IRAM_ATTR isrKey2();
void IRAM_ATTR isrKey3();
void IRAM_ATTR isrKey4();

// Helper for keypad
void processKey(uint8_t pin,
                volatile bool &interruptFlag,
                uint32_t &lastPressMs,
                void (*handler)());

// =============================
// Setup
// =============================

void setup() {
  // 1. Initialize serial port for debug output
  setupSerial();

  // 2. Initialize OLED display
  setupDisplay();

  // 3. Initialize IR sensors (pin modes, ADC)
  setupSensors();

  // 4. Initialize keypad (pin modes, interrupts)
  setupKeypad();

  // 5. Initialize RTC I2C bus and DS3231 RTC
  rtcWire.begin(RTC_SDA_PIN, RTC_SCL_PIN, 100000); // 100kHz
  rtc.begin(&rtcWire);

  // 6. Time source selection logic: Try NTP, else RTC, else pause system
  g_timeSource = TIME_NONE;
  g_timeValid = false;
  strcpy(g_timeErrorMsg, "");

  // 7. Print RTC time at boot for debugging
  DateTime rtcNowDebug = rtc.now();
  Serial.print("[DEBUG] RTC time at boot: ");
  Serial.print(rtcNowDebug.year()); Serial.print("-");
  Serial.print(rtcNowDebug.month()); Serial.print("-");
  Serial.print(rtcNowDebug.day()); Serial.print(" ");
  Serial.print(rtcNowDebug.hour()); Serial.print(":");
  Serial.print(rtcNowDebug.minute()); Serial.print(":");
  Serial.println(rtcNowDebug.second());

  // 8. Try to get NTP time (5s timeout)
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");  //auto sync time from NTP in background
  time_t ntpEpoch = time(nullptr);
  const time_t validThreshold = 1000000000;
  unsigned long startNtp = millis();
  while (ntpEpoch < validThreshold && (millis() - startNtp) < 5000) { // 5s max wait
    delay(100);
    ntpEpoch = time(nullptr);
  }
  Serial.print("[DEBUG] NTP time at boot: ");
  if (ntpEpoch >= validThreshold) {
    struct tm ntpTm;
    gmtime_r(&ntpEpoch, &ntpTm);
    char ntpBuf[32];
    strftime(ntpBuf, sizeof(ntpBuf), "%Y-%m-%d %H:%M:%S UTC", &ntpTm);
    Serial.println(ntpBuf);
  } else {
    Serial.println("NTP not available");
  }

  // 9. Set time source and program RTC if needed
  if (ntpEpoch >= validThreshold) {
    g_timeSource = TIME_NTP;
    g_timeValid = true;
    Serial.println("[TIME] NTP time acquired at boot.");
    logCurrentISTTime();
    // Set RTC from NTP if RTC is invalid or drift > 2min (done later)
  } else {
    // 2. Try RTC
    DateTime rtcNow = rtc.now();
    if (isRTCValid(rtcNow)) {
      setSystemTimeFromRTC(rtcNow);
      g_timeSource = TIME_RTC;
      g_timeValid = true;
      Serial.println("[TIME] RTC used for system time at boot.");
      logCurrentISTTime();
    } else {
      // 3. Neither NTP nor RTC valid
      g_timeSource = TIME_NONE;
      g_timeValid = false;
      strcpy(g_timeErrorMsg, "No valid NTP or RTC time! System paused.");
      Serial.println("[TIME] ERROR: No valid NTP or RTC time! System paused.");
    }
  }

  // 10. (Optional) One-time RTC initialization from NTP if FIRSTTIME is defined
#ifdef FIRSTTIME
  Serial.println("[RTC] FIRSTTIME: Setting DS3231 from NTP (UTC)...");
  setRTCFromNTP();
#endif

  // 11. Restore persistent storage (token count, meal, date)
  Serial.println("[STORAGE] Restoring token data from preferences...");
  prefs.begin("tokencfg", true); // read-only
  token_data.token_count = prefs.getInt("token_count", 0);
  token_data.meal = (mealType)prefs.getInt("meal", NONE);
  String dateStr = prefs.getString("date", "1970-01-01");
  strncpy(token_data.date, dateStr.c_str(), sizeof(token_data.date));
  token_data.date[10] = '\0';
  prefs.end();
  bool valid = true;
  if (token_data.meal < NONE || token_data.meal >= MEAL_COUNT) valid = false;
  if (strlen(token_data.date) != 10) valid = false;
  Serial.printf("[STORAGE] Restored token_data: count=%d, meal=%d, date=%s\n",
                token_data.token_count, (int)token_data.meal, token_data.date);
                g_tokenCount= token_data.token_count;
  if (!valid) {
    token_data.token_count = 0;
    token_data.meal = NONE;
    strcpy(token_data.date, "1970-01-01");
    token_data.update = false;
    prefs.begin("tokencfg", false);
    prefs.putInt("token_count", token_data.token_count);
    prefs.putInt("meal", token_data.meal);
    prefs.putString("date", token_data.date);
    prefs.end();
    Serial.println("[STORAGE] Invalid token data in prefs, reset to defaults.");
  }

  // 12. WiFiManager: connect or launch captive portal if needed
  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setWiFiAutoReconnect(true);
  String setupName = String(deviceId) + "_SETUP";
  if (!wm.autoConnect(setupName.c_str())) {
    delay(1000);
    ESP.restart();
  }

  // 13. Wait for NTP sync and initialize meal/date state
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  while (true) {
    timeManager.begin(TIME_SYNC_INTERVAL_MS, TIME_INITIAL_TIMEOUT_MS);
    time_t nowEpoch = timeManager.now();
    if (nowEpoch >= 1000000000) {
      struct tm tinfo;
      timeManager.getUTCTime(tinfo);
      char buf[32];
      strftime(buf, sizeof(buf), "%FT%TZ", &tinfo);
      // Convert NTP to IST date string
      time_t epochLocal = nowEpoch + IST_OFFSET_SECONDS;
      struct tm tIST;
      gmtime_r(&epochLocal, &tIST);
      char todayIST[11];
      strftime(todayIST, sizeof(todayIST), "%Y-%m-%d", &tIST);
      int hour = tIST.tm_hour + DUMMYHREFORTESTING;
      mealType currentMealType = NONE;
      if (hour >= BFL && hour <= BFH) currentMealType = BREAKFAST;
      else if (hour >= LFL && hour <= LFH) currentMealType = LUNCH;
      else if (hour >= DFL && hour <= DFH) currentMealType = DINNER;
      if (currentMealType == NONE) {
        // Not a meal time, don't initialize token_data
        Serial.println("[STORAGE] Not in meal time window, token count not modified.");
      } else if (strcmp(token_data.date, todayIST) == 0 && token_data.meal == currentMealType) {
        // Keep token_count
        Serial.println("[STORAGE] Same date and meal, keeping token count.");
      } else {
        Serial.println("[STORAGE] New date or meal, resetting token count to 0.");
        token_data.token_count = 0;
        strncpy(token_data.date, todayIST, sizeof(token_data.date));
        token_data.date[10] = '\0';
        token_data.meal = currentMealType;
        token_data.update = true;
        prefs.begin("tokencfg", false);
        prefs.putInt("token_count", token_data.token_count);
        prefs.putInt("meal", token_data.meal);
        prefs.putString("date", token_data.date);
        prefs.end();
      }
      break;
    } else {
      delay(2000);
    }
  }

  // 14. Create a queue for up to 3 send jobs (for HTTP background task)
  g_sendQueue = xQueueCreate(3, sizeof(SendJob));
  if (g_sendQueue == nullptr) {
    Serial.println("ERROR: Failed to create send queue!");
  } else {
    // Create background HTTP sender task on core 1 (typical for Arduino loop on core 1)
    xTaskCreatePinnedToCore(
      httpSenderTask,      // task function
      "httpSender",        // name
      8192,                // stack size (bytes)
      nullptr,             // parameter
      1,                   // priority (low/medium)
      nullptr,             // task handle
      1                    // core ID (0 or 1; often 1 works fine)
    );
  }

  // 15. Final system ready message
  Serial.println("System initialized.");
}

// =============================
// Main Loop
// =============================

void loop() {
  // --- Periodic RTC/NTP drift correction ---
  if (g_timeValid && g_timeSource == TIME_NTP) {
    if (now - lastRTCDriftCheck > RTC_NTP_DRIFT_CHECK_INTERVAL_MS) {
      lastRTCDriftCheck = now;
      DateTime rtcNow = rtc.now();
      time_t ntpEpoch = time(nullptr);
      if (!isRTCValid(rtcNow)) {
        rtc.adjust(DateTime(ntpEpoch));
        Serial.println("[RTC] RTC was invalid, set from NTP.");
        logCurrentISTTime();
      } else {
        long drift = llabs((long)ntpEpoch - (long)rtcNow.unixtime());
        if (drift > RTC_NTP_DRIFT_THRESHOLD_SEC) {
          rtc.adjust(DateTime(ntpEpoch));
          Serial.printf("[RTC] RTC drifted by %ld sec (>2min), updated from NTP.\n", drift);
          logCurrentISTTime();
        } else {
          Serial.printf("[RTC] RTC drift %ld sec, within threshold. No update.\n", drift);
        }
      }
    }
  }

  now = millis();

  // WiFi check every 10s
  if (now - lastWiFiCheck > 10000) {
    lastWiFiCheck = now;
    checkWiFi();
  }
  timeManager.update();


  // --- Robust time source check ---
  if (!g_timeValid) {
    // No valid time source: show error, pause all logic
    g_timeString = "--:-- ERR";
    // Optionally display error on OLED
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_10x20_tf);
    u8g2.setCursor(0, 24);
    u8g2.print("TIME ERROR");
    u8g2.setFont(u8g2_font_7x13B_tf);
    u8g2.setCursor(0, 44);
    u8g2.print(g_timeErrorMsg);
    u8g2.sendBuffer();
    delay(LOOP_DELAY_MS);
    return;
  }

  // --- Normal operation: valid time source ---
  // Get IST time for meal logic and display
  time_t epoch = time(nullptr);
  time_t epochLocal = epoch + IST_OFFSET_SECONDS;
  struct tm t;
  gmtime_r(&epochLocal, &t);
  char dateStr[11];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &t);
  int hour = t.tm_hour + DUMMYHREFORTESTING;
  strncpy(token_data.date, dateStr, sizeof(token_data.date));
  token_data.date[10] = '\0';

  // Format time string for display
  char timeBuf[10];
  int displayHour = t.tm_hour;
  bool isPM = false;
  if (displayHour >= 12) { isPM = true; if (displayHour > 12) displayHour -= 12; }
  if (displayHour == 0) displayHour = 12;
  snprintf(timeBuf, sizeof(timeBuf), "%02d:%02d %s", displayHour, t.tm_min, isPM ? "PM" : "AM");
  g_timeString = String(timeBuf);

  // Determine meal window
  if (hour >= BFL && hour < BFH) token_data.meal = BREAKFAST;
  else if (hour >= LFL && hour <= LFH) token_data.meal = LUNCH;
  else if (hour >= DFL && hour <= DFH) token_data.meal = DINNER;
  else token_data.meal = NONE;

  // IR sensor logic and token counting
  readSensors(); // This will increment g_tokenCount on valid IR event
  // Sync g_tokenCount to token_data if in meal window
  if (token_data.meal != NONE) {
    token_data.token_count = g_tokenCount;
    // token_data2 = token_data;
    // token_data3 = token_data;
    // token_data2.token_count += 2;
    // token_data3.token_count += 3;

    // Send to backend every 10s
    // Enqueue sends every 10s (non-blocking for main loop)
    if (((now - lastSensorSend) >= TIME_SYNC_DATA_INTERVAL_MS) ||((g_tokenCount != g_tokenCountPrevious) && (now - lastSensorSend) >= 1000)) {   // 10,000 ms = 10 s
      lastSensorSend = now;
      g_tokenCountPrevious = g_tokenCount;
      Serial.println("updated");
      if (g_sendQueue != nullptr) {
        SendJob job1{ deviceId,  token_data };
        // If queue is full, remove the oldest job before sending the new one
        if (uxQueueSpacesAvailable(g_sendQueue) == 0) {
          SendJob dummy;
          xQueueReceive(g_sendQueue, &dummy, 0); // remove oldest job
        }
        xQueueSend(g_sendQueue, &job1, 0);
        // Repeat for other jobs if needed
        // SendJob job2{ deviceId2, token_data2 };
        // if (uxQueueSpacesAvailable(g_sendQueue) == 0) { xQueueReceive(g_sendQueue, &dummy, 0); }
        // xQueueSend(g_sendQueue, &job2, 0);
        // SendJob job3{ deviceId3, token_data3 };
        // if (uxQueueSpacesAvailable(g_sendQueue) == 0) { xQueueReceive(g_sendQueue, &dummy, 0); }
        // xQueueSend(g_sendQueue, &job3, 0);
      }
    }
  }

  // EEPROM save every 20s or if update flag set
  if ((now - lastEEPROMWrite >= 10000) || token_data.update == true) {
    lastEEPROMWrite = now;
    token_data.update = false;
    prefs.begin("tokencfg", false);
    prefs.putInt("token_count", token_data.token_count);
    prefs.putInt("meal", token_data.meal);
    prefs.putString("date", token_data.date);
    prefs.end();
  }

  // readSensors();   // analog-based token detection + Serial debug
  handleKeypad();  // keypad + bundle/reset logic
  updateDisplay(); // draw status bar + big counter
  delay(LOOP_DELAY_MS);
}

// =============================
// Setup Functions
// =============================

void setupSerial() {
  Serial.begin(460800);
  delay(200);
  Serial.println();
  Serial.println("=== ESP32 Keypad + TCRT5000 + OLED (U8g2) ===");
}

void setupDisplay() {
  u8g2.begin();
  // Default font can be small; specific fonts are set in drawScreen()
  u8g2.setFont(u8g2_font_7x13_tf);
}

void setupSensors() {
  pinMode(S1_D0_PIN, INPUT);
  pinMode(S2_D0_PIN, INPUT);
  analogReadResolution(12);  // 0..4095 on ESP32
}

void setupKeypad() {
  pinMode(KEY1_PIN, INPUT_PULLUP);
  pinMode(KEY2_PIN, INPUT_PULLUP);
  pinMode(KEY3_PIN, INPUT_PULLUP);
  pinMode(KEY4_PIN, INPUT_PULLUP);

  attachInterrupt(digitalPinToInterrupt(KEY1_PIN), isrKey1, FALLING);
  attachInterrupt(digitalPinToInterrupt(KEY2_PIN), isrKey2, FALLING);
  attachInterrupt(digitalPinToInterrupt(KEY3_PIN), isrKey3, FALLING);
  attachInterrupt(digitalPinToInterrupt(KEY4_PIN), isrKey4, FALLING);
}

// =============================
// Sensor Handling (analog-based Schmitt trigger)
// =============================

void readSensors() {
  uint32_t now = millis();

  // Sample analog IR only every IR_SAMPLE_INTERVAL_MS
  if (now - g_lastIrSampleMs >= IR_SAMPLE_INTERVAL_MS) {
    g_lastIrSampleMs = now;

    // Analog reads
    g_sensor1Analog = analogRead(S1_A0_PIN);
    g_sensor2Analog = analogRead(S2_A0_PIN);

    // Optional digital reads (for debug only, NOT used for counting)
    g_sensor1Digital = digitalRead(S1_D0_PIN);
    g_sensor2Digital = digitalRead(S2_D0_PIN);

    // ----- Schmitt trigger & event detection for Sensor 1 -----
    bool s1Event = false;
    if (g_s1HighRegion) {
      // High region → look for dip below low threshold (Lth)
      if (g_sensor1Analog <= IR_LTH) {
        s1Event = true;           // local event on S1
        g_s1HighRegion = false;   // move to low region
      }
    } else {
      // Low region → wait to go back above high threshold (Hth) to re-arm
      if (g_sensor1Analog >= IR_HTH) {
        g_s1HighRegion = true;
      }
    }

    // ----- Schmitt trigger & event detection for Sensor 2 -----
    bool s2Event = false;
    if (g_s2HighRegion) {
      if (g_sensor2Analog <= IR_LTH) {
        s2Event = true;           // local event on S2
        g_s2HighRegion = false;   // move to low region
      }
    } else {
      if (g_sensor2Analog >= IR_HTH) {
        g_s2HighRegion = true;
      }
    }

    // OR logic between sensors with merge window
    bool tokenEvent = s1Event || s2Event;
    if (tokenEvent) {
      // If this event is sufficiently separated from the last one,
      // treat it as a new token. Otherwise, same token across two sensors.
      if (now - g_lastTokenEventMs >= TOKEN_MERGE_WINDOW_MS) {
        g_lastTokenEventMs = now;

        if (g_addBundle10) {
          g_tokenCount += 10;
          g_addBundle10 = false;  // one-shot bundle
          Serial.println("Token event: +10 (bundle)");
        } else {
          g_tokenCount += 1;
          // Serial.println("Token event: +1");
        }

        if (g_tokenCount > 9999) {
          g_tokenCount = 9999;
        }
      } else {
        // Within merge window: treat as same physical token → ignore for count
        Serial.println("Token event merged (same token across sensors)");
      }
    }

    // Serial debug output (for plotting / diagnostics)
    // Serial.print(">");
    // Serial.print("S1A:");
    // Serial.print(g_sensor1Analog);
    // Serial.print(",D1:");
    // Serial.print(g_sensor1Digital);
    // Serial.print(",S2A:");
    // Serial.print(g_sensor2Analog);
    // Serial.print(",D2:");
    // Serial.print(g_sensor2Digital);
    // Serial.print(",CNT:");
    // Serial.println(g_tokenCount);
  }
}

// =============================
// Keypad Handling
// =============================

void processKey(uint8_t pin,
                volatile bool &interruptFlag,
                uint32_t &lastPressMs,
                void (*handler)()) {
  if (!interruptFlag) {
    return;
  }

  // Clear flag early
  interruptFlag = false;

  uint32_t now = millis();
  if (now - lastPressMs < BUTTON_DEBOUNCE_MS) {
    // Debounce reject
    return;
  }

  // Confirm button is still pressed (active LOW)
  if (digitalRead(pin) == LOW) {
    lastPressMs = now;
    handler();
  }
}

void handleKeypad() {
  processKey(KEY1_PIN, g_key1Interrupt, g_key1LastPressMs, onKey1Pressed);
  processKey(KEY2_PIN, g_key2Interrupt, g_key2LastPressMs, onKey2Pressed);
  processKey(KEY3_PIN, g_key3Interrupt, g_key3LastPressMs, onKey3Pressed);
  processKey(KEY4_PIN, g_key4Interrupt, g_key4LastPressMs, onKey4Pressed);
}

// =============================
// Button Press Handlers
// =============================

// Key 1 → Reset token counter
void onKey1Pressed() {
  g_tokenCount = 0;
  g_lastKeyPressed = "Key 1 Reset";
  Serial.println("Key 1 pressed -> Counter RESET");
}

// Key 2 → Enable +10 bundle for next token event
void onKey2Pressed() {
  g_addBundle10 = true;
  g_lastKeyPressed = "Key 2 Bundle +10";
  Serial.println("Key 2 pressed -> Next token = +10 bundle");
}

// Key 3 → Reserved
void onKey3Pressed() {
  g_lastKeyPressed = "Key 3 (reserved)";
  Serial.println("Key 3 pressed (reserved)");
}

// Key 4 → Reserved
void onKey4Pressed() {
  g_lastKeyPressed = "Key 4 (reserved)";
  Serial.println("Key 4 pressed (reserved)");
}

// =============================
// Display Handling (U8g2)
// =============================

void updateDisplay() {
  u8g2.clearBuffer();
  drawScreen();
  u8g2.sendBuffer();
}

// Draw 5-segment battery at top-right
// levelIndex: 0..4 → 0%,25%,50%,75%,100%
void drawBattery(uint8_t levelIndex) {
  if (levelIndex > 4) levelIndex = 4;   // clamp

  // Battery position & size (top-right)
  const uint8_t battW = 18;    // main body width
  const uint8_t battH = 8;     // main body height
  const uint8_t tipW  = 2;     // small terminal width

  const uint8_t battX = 128 - battW - tipW - 1; // leave 1px margin from right
  const uint8_t battY = 2;                       // a bit below top border

  // Outline of battery body
  u8g2.drawFrame(battX, battY, battW, battH);

  // Battery terminal ("+" side)
  u8g2.drawBox(battX + battW, battY + 2, tipW, battH - 4);

  // Inner filling (5 bars, we’ll light 0..levelIndex)
  const uint8_t innerX = battX + 2;
  const uint8_t innerY = battY + 2;
  const uint8_t innerH = battH - 4;
  const uint8_t barW   = 2;
  const uint8_t gap    = 1;

  for (uint8_t i = 0; i <= levelIndex; i++) {
    uint8_t bx = innerX + i * (barW + gap);
    u8g2.drawBox(bx, innerY, barW, innerH);
  }
}

// Draw a WiFi/signal icon as vertical bars left of the battery.
// levelIndex: 0..4
//  0 -> circle with slash (no network)
//  1..4 -> that many bars (like 4G)
void drawWifi(uint8_t levelIndex) {
  if (levelIndex > 4) levelIndex = 4;

  // Position based on battery so layout stays aligned
  const uint8_t battW = 18;
  const uint8_t tipW  = 2;
  const uint8_t battX = 128 - battW - tipW - 1;

  const uint8_t wifiW = 12;
  const uint8_t wifiH = 10;
  const uint8_t wifiX = battX - wifiW - 3;  // small gap (3 px) left of battery
  const uint8_t wifiY = 1;

  if (levelIndex == 0) {
    // --- No network: circle with slash ---
    uint8_t cx = wifiX + wifiW / 2;
    uint8_t cy = wifiY + wifiH / 2;
    uint8_t r  = (wifiW < wifiH ? wifiW : wifiH) / 2 - 1;

    // Circle outline
    u8g2.drawCircle(cx, cy, r);

    // Slash: diagonal line across circle
    u8g2.drawLine(cx - r, cy - r, cx + r, cy + r);
    return;
  }

  // --- Signal bars (1..4) ---
  const uint8_t bars  = 4;
  const uint8_t barW  = 2;
  const uint8_t gap   = 1;
  const uint8_t baseY = wifiY + wifiH - 1; // bottom baseline

  // Right-aligned bars: smallest on left, tallest on right (like cellular icon)
  for (uint8_t i = 0; i < bars; i++) {
    uint8_t barIndex = i + 1; // 1..4
    if (barIndex > levelIndex) {
      continue; // don't draw bars above the current level
    }

    // Height increments: 3, 5, 7, 9 pixels
    uint8_t barH = 3 + (2 * i);

    // From right to left
    uint8_t xRight = wifiX + wifiW - 1 - (bars - 1 - i) * (barW + gap);
    uint8_t xLeft  = xRight - barW + 1;
    uint8_t yTop   = baseY - barH + 1;

    u8g2.drawBox(xLeft, yTop, barW, barH);
  }
}

void drawScreen() {
  const int16_t x0 = 0;
  int16_t y = 14;

  // Dynamic header: D/M/T based on deviceId
  char titleChar = 'D';
  if (strcmp(deviceId, "uno_2") == 0) titleChar = 'M';
  else if (strcmp(deviceId, "uno_3") == 0) titleChar = 'T';
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.setCursor(x0, y);
  u8g2.print(titleChar);

  drawBattery(g_batteryLevelIndex);
  drawWifi(g_wifiLevelIndex);

  u8g2.setFont(u8g2_font_7x13B_tf);
  const uint8_t battW = 18;
  const uint8_t tipW  = 2;
  const uint8_t wifiW = 12;
  int16_t battX = 128 - battW - tipW - 1;
  int16_t wifiX = battX - wifiW - 3;
  int16_t timeWidth = u8g2.getStrWidth(g_timeString.c_str());
  int16_t timeX = wifiX - timeWidth - 3;
  if (timeX < 0) timeX = 0;
  u8g2.setCursor(timeX, y - 3);
  u8g2.print(g_timeString);

  // Main area: big token counter 0..9999
  u8g2.setFont(u8g2_font_logisoso32_tf);
  int displayCount = g_tokenCount;
  if (displayCount < 0) displayCount = 0;
  if (displayCount > 9999) displayCount = 9999;
  char buf[6];
  snprintf(buf, sizeof(buf), "%d", displayCount);
  int16_t countWidth = u8g2.getStrWidth(buf);
  int16_t countX = (128 - countWidth) / 2;
  if (countX < 0) countX = 0;
  int16_t countY = 64 - 6;
  u8g2.setCursor(countX, countY);
  u8g2.print(buf);
}

// =============================
// ISR Implementations
// =============================

void IRAM_ATTR isrKey1() {
  g_key1Interrupt = true;
}

void IRAM_ATTR isrKey2() {
  g_key2Interrupt = true;
}

void IRAM_ATTR isrKey3() {
  g_key3Interrupt = true;
}

void IRAM_ATTR isrKey4() {
  g_key4Interrupt = true;
}


// Helper: Check if RTC time is valid (not 1970 or too old)
inline bool isRTCValid(const DateTime& dt) {
  return dt.year() > 2024;
}

// Helper: Set system time from RTC (UTC)
inline void setSystemTimeFromRTC(const DateTime& dt) {
  struct timeval tv;
  tv.tv_sec = dt.unixtime();
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

// Helper: Set RTC from system time (UTC)
inline void setRTCFromSystemTime() {
  time_t nowEpoch = time(nullptr);
  rtc.adjust(DateTime(nowEpoch));
}

// Helper: Print current IST time string to Serial
inline void logCurrentISTTime() {
  time_t epoch = time(nullptr) + IST_OFFSET_SECONDS;
  struct tm t;
  gmtime_r(&epoch, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S IST", &t);
  Serial.print("[TIME] Current IST: ");
  Serial.println(buf);
}


// One-time RTC initialization from NTP
#ifdef FIRSTTIME
void setRTCFromNTP() {
  // Wait for NTP to be valid
  time_t ntpEpoch = time(nullptr);
  const time_t validThreshold = 1000000000;
  unsigned long start = millis();
  while (ntpEpoch < validThreshold && (millis() - start) < 15000) { // 15s max wait
    delay(100);
    ntpEpoch = time(nullptr);
  }
  if (ntpEpoch >= validThreshold) {
    rtc.adjust(DateTime(ntpEpoch));
    Serial.println("[RTC] DS3231 set from NTP (UTC) for first time.");
  } else {
    Serial.println("[RTC] ERROR: NTP not available, RTC not set.");
  }
}
#endif