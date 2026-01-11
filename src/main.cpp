

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <Preferences.h>
#include <time.h>
#include <RTClib.h>



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

// Supabase backend config
// Device ID macros for build-time selection
#define TIFFIN
// #define MAHATMA
#if defined(TIFFIN)
#define DEVICE_ID   "uno_2"
#define PEER1_ID    "uno_1"
#define PEER2_ID    "uno_3"
#define DEVICE_CAL  0.985
#elif defined(MAHATMA)
#define DEVICE_ID   "uno_3"
#define PEER1_ID    "uno_1"
#define PEER2_ID    "uno_2"
#define DEVICE_CAL  0.9797
#else // Default: DARSHANARTHI
#define DEVICE_ID   "uno_1"
#define PEER1_ID    "uno_2"
#define PEER2_ID    "uno_3"
#define DEVICE_CAL  0.985
#endif

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
// Charger detect pin
#define CHARGER_DETECT_PIN 16
boolean chargerState = false;
void checkChargerStatus() {
  chargerState = digitalRead(CHARGER_DETECT_PIN);
  // if (chargerState == HIGH) {
  //   Serial.println("charging on");
  // } else {
  //   Serial.println("charging off");
  // }
}
// Battery voltage sense pin (divider output)
#define VBAT_SENSE_PIN 34
#define VBAT_DIVIDER_RATIO 2.0f // 2x100K divider: Vbat/2 at pin
#define VBAT_ADC_MAX 4095.0f
#define VBAT_REF_VOLTAGE 2.2f // ESP32 ADC reference voltage
float vBat = 0.0f;

float readBatteryVoltage() {
  vBat = analogReadMilliVolts(VBAT_SENSE_PIN)*VBAT_DIVIDER_RATIO/1000.0*DEVICE_CAL;
  // uint32_t mv = analogReadMilliVolts(VBAT_SENSE_PIN);
// Serial.printf("[ADC] Vsense=%lu mV, Vbat=%.3f V\n", mv, (mv*2.0)/1000.0);
  return vBat;
}


// Keypad (4 buttons)
#define KEY1_PIN  5    // Button "1"
#define KEY2_PIN  17   // Button "2"
#define KEY3_PIN  19   // Button "3"
#define KEY4_PIN  18   // Button "4"


// TCRT5000 Sensor 1
#define S1_D0_PIN 23   // Digital (not used for counting)
#define S1_A0_PIN 32   // Analog (used for counting)

// TCRT5000 Sensor 2
#define S2_A0_PIN 33   // Analog (used for counting)

// TCRT5000 Sensor 3 (NEW)
#define S3_A0_PIN 35   // Analog (used for counting)
// D0 not used for S3

// I2C pins for ESP32 (OLED) - hardware default: SDA=21, SCL=22

// =============================
// Config Macros
// =============================

#define BUTTON_DEBOUNCE_MS     100UL   // debounce time for keypad (in ms)
#define LOOP_DELAY_MS          1UL    // main loop delay, keep small for fast sampling

// NEW: Deterministic sensor task period (ms)
#define SENSOR_TASK_PERIOD_MS  5UL

// Analog sampling and Schmitt trigger thresholds for IR
#define IR_SAMPLE_INTERVAL_MS  1UL    // sample analog inputs every 1ms
#define IR_LTH                 2500   // low threshold for Schmitt trigger
#define IR_HTH                 3000   // high threshold for Schmitt trigger

// Time window for OR-ing between two sensors (ms)
#define TOKEN_MERGE_WINDOW_MS  250UL    // if events are closer than this, count as one token

// Hold time for combo reset: Keys 1 + 4 pressed simultaneously
#define COMBO_RESET_HOLD_MS    1000UL

// Meal window macros (IST)
#define BFL 6
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

// =============================
// Static WiFi fallback networks (2 SSIDs)
// =============================
#define STATIC_SSID1   "Unodari"
#define STATIC_PASS1   "s1mandhar"

#define STATIC_SSID2   "Unodari.123"
#define STATIC_PASS2   "dadaniruma"

// Custom SSID stored in Preferences (3rd network option)
char g_customSSID[33] = "";
char g_customPass[65] = "";

// WiFi Portal state
static bool g_portalActive = false;
static bool g_portalStopRequested = false;  // Flag to request stop from handler
static WebServer* g_portalServer = nullptr;
static DNSServer* g_dnsServer = nullptr;
static uint32_t g_portalStartMs = 0;
#define PORTAL_TIMEOUT_MS 120000  // 2 minutes timeout for portal


// India Standard Time offset from UTC in seconds (+5:30)
static const long IST_OFFSET_SECONDS = 5 * 3600 + 30 * 60;
volatile unsigned long now=0; // current time in loop
// volatile unsigned long lastWiFiCheck = 0;
volatile unsigned long lastEEPROMWrite = 0;
volatile unsigned long lastSensorSend = 0;



// =============================
// Global State
// =============================


// Sensor readings (for debug / plotting)
int  g_sensor1Analog = 0;
int  g_sensor2Analog = 0;
int  g_sensor3Analog = 0;      // NEW: Sensor 3 analog
bool g_sensor1Digital = false;
bool g_sensor2Digital = false;
// No digital for sensor 3

// Schmitt trigger state for each sensor (true = high region, false = low region)
bool g_s1HighRegion = true;
bool g_s2HighRegion = true;
bool g_s3HighRegion = true;    // NEW: Sensor 3 Schmitt state

// Token counter 0..9999
int  g_tokenCount = 0;
// One-shot bundle size for the next token event. 0 = normal +1.
uint8_t g_bundleAdd = 0;
// Bundle mode lock state: true = locked, false = unlocked
bool g_bundleLocked = true;
// For 2+3 hold detection
static uint32_t g_bundleLockComboStartMs = 0;
static bool g_bundleLockComboActive = false;
// Timestamp when bundle was set (for timeout)
uint32_t g_bundleSetMs = 0;

// Track previous token count for immediate HTTP send
int g_tokenCountPrevious = 0;

// Last time we sampled the analog IR (for 1ms sampling)
uint32_t g_lastIrSampleMs = 0;

// Last time any token event was counted (for OR-window between sensors)
uint32_t g_lastTokenEventMs = 0;

// Last pressed key info (for debug/Serial/logging)
String g_lastKeyPressed = "None";

// =============================
// Sensor Task Metrics (timing/health)
// =============================
volatile uint32_t g_sensorCallsTotal = 0;     // total invocations since boot
volatile uint32_t g_sensorCallsPerSec = 0;    // rolling count per second
volatile uint32_t g_sensorLastDtMs = 0;       // last inter-call dt in ms
volatile uint32_t g_sensorMaxDtMs = 0;        // max dt observed in the last reporting window
volatile uint32_t g_sensorDeadlineMisses = 0; // count of dt exceeding expected period+margin

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


const char *deviceId = DEVICE_ID;
const char *deviceId2 = PEER1_ID;
const char *deviceId3 = PEER2_ID;
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

// --- Peer fetch request struct and queue ---
typedef struct {
  char peerId[16];
  char dateStr[12];
  mealType meal;
  TokenData* peerData;
} PeerFetchRequest;

QueueHandle_t peerFetchQueue = nullptr;
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
void fetchPeer(const char* peerId, TokenData* peerData, const char* apikey, mealType meal, const char* dateStr);
void peerFetchTask(void* pv);
void fetchPeerDataIfNeeded();
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
inline void setSystemTimeFromEpoch(time_t e);
static bool tryAcquireTimeFromNTP();
static uint8_t batteryLevelFromVoltageHyst(float vBat, uint8_t curLevel);
void sensorTask(void *pv);
// WiFi simplified functions
void loadCustomCredentials();
void saveCustomCredentials(const char* ssid, const char* pass);
void startWiFiPortal();
void stopWiFiPortal();
void handlePortalRoot();
void handlePortalSave();
void processPortal();
bool tryConnectToNetworks();
void wifiConnectTask(void *pv);


void httpSenderTask(void *pvParameters) ;
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


static void showBootMessage(const char* line1, const char* line2 = nullptr) {
  u8g2.clearBuffer();
  u8g2.setFont(u8g2_font_8x13B_tf);
  u8g2.drawStr(0, 16, line1);
  if (line2) u8g2.drawStr(0, 34, line2);
  u8g2.sendBuffer();
}



void setup() {
    // --- Peer fetch queue and task ---
    if (peerFetchQueue == nullptr) {
      peerFetchQueue = xQueueCreate(2, sizeof(PeerFetchRequest));
    }
    xTaskCreatePinnedToCore(
      peerFetchTask,
      "peerFetchTask",
      16384,
      nullptr,
      1, // low/medium priority
      nullptr,
      1 // core 1
    );
  // 1. Initialize serial port for debug output
  setupSerial();

  // 2. Initialize OLED display
  setupDisplay();
showBootMessage("Powering ON...", "Please wait");
delay(300);   // optional (0.5–1.0s feels good)
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

  // Load custom WiFi credentials from storage
  loadCustomCredentials();

  // Start WiFi connection task (non-blocking)
  xTaskCreatePinnedToCore(
    wifiConnectTask,
    "wifi_connect",
    4096,
    nullptr,
    1,
    nullptr,
    0
  );

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
  while (ntpEpoch < validThreshold && (millis() - startNtp) < 3000) { // 0.5s max wait
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
  // ✅ Set RTC immediately at boot (recommended)
  setSystemTimeFromEpoch(ntpEpoch);
  rtc.adjust(DateTime(ntpEpoch));

  Serial.println("[TIME] NTP time acquired at boot.");
  logCurrentISTTime();
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
// 3. Neither NTP nor RTC valid at boot
    g_timeSource = TIME_NONE;
    g_timeValid  = false;
    strcpy(g_timeErrorMsg, "Waiting for NTP...");
    Serial.println("[TIME] No valid time at boot -> waiting for WiFi/NTP in loop.");
    }
  }



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

  // 12. WiFi: connection handled by wifiConnectTask (manual portal via Key4 long-press)

  // 13. Create a queue for up to 3 send jobs (for HTTP background task)
  g_sendQueue = xQueueCreate(3, sizeof(SendJob));
  if (g_sendQueue == nullptr) {
    Serial.println("ERROR: Failed to create send queue!");
  } else {
    // Create background HTTP sender task on core 1
    xTaskCreatePinnedToCore(
      httpSenderTask,
      "httpSender",
      8192,
      nullptr,
      1,
      nullptr,
      1
    );
  }

  // 14. Start sensor task for deterministic 2ms sampling
  xTaskCreatePinnedToCore(
    sensorTask,
    "sensorTask",
    4096,
    nullptr,
    2,      // priority higher than WiFi/http tasks
    nullptr,
    1       // run on core 1
  );

  Serial.println("System initialized.");

  analogReadResolution(12);
  analogSetPinAttenuation(VBAT_SENSE_PIN, ADC_11db);
}

// =============================
// Main Loop
// =============================

void loop() {

  // ---- Loop stack high-water mark monitor (1 Hz) ----
//   static uint32_t lastStackPrintMs = 0;
//   uint32_t nowMs = millis();

//   if (nowMs - lastStackPrintMs >= 1000) {
//     lastStackPrintMs = nowMs;
// time_t epoch = time(nullptr);
// struct tm utc;
// gmtime_r(&epoch, &utc);

// time_t istEpoch = epoch + 19800;
// struct tm ist;
// gmtime_r(&istEpoch, &ist);

// Serial.printf("[CHK] UTC=%02d:%02d  IST=%02d:%02d\n",
//               utc.tm_hour, utc.tm_min, ist.tm_hour, ist.tm_min);

// //     // High-water mark is in WORDS (4 bytes per word)
// //     UBaseType_t hw = uxTaskGetStackHighWaterMark(NULL);

// //     Serial.printf("loopTask HW=%u words (%u bytes free)\n",
// //                   hw,
// //                   hw * sizeof(StackType_t));
// // Serial.printf("loop HW=%u words, heap=%u, minHeap=%u\n",
// //               uxTaskGetStackHighWaterMark(NULL),
// //               ESP.getFreeHeap(),
// //               ESP.getMinFreeHeap());

//   }

          fetchPeerDataIfNeeded();
        // --- Update currentMeal based on meal window logic ---
        int hour = 0;
        time_t epoch = time(nullptr) + 19800; // IST offset +5:30
        struct tm t;
        gmtime_r(&epoch, &t);
        hour = t.tm_hour;
        mealType newMeal = NONE;
        if (hour >= BFL && hour < BFH) newMeal = BREAKFAST;
        else if (hour >= LFL && hour <= LFH) newMeal = LUNCH;
        else if (hour >= DFL && hour <= DFH) newMeal = DINNER;
        else newMeal = NONE;
        currentMeal = newMeal;

      // --- Bundle timeout: if bundle set and expired, reset to default ---
      if (g_bundleAdd > 0 && (millis() - g_bundleSetMs > 5000)) {
        g_bundleAdd = 0;
      }
    // --- Update WiFi signal strength bar (g_wifiLevelIndex) based on RSSI ---
    if (WiFi.status() == WL_CONNECTED) {
      long rssi = WiFi.RSSI();
      // Map RSSI to 4 bars: 0 = no network, 1 = weak, 2 = fair, 3 = good, 4 = excellent
      if (rssi >= -55) g_wifiLevelIndex = 4;         // Excellent
      else if (rssi >= -65) g_wifiLevelIndex = 3;    // Good
      else if (rssi >= -75) g_wifiLevelIndex = 2;    // Fair
      else if (rssi >= -85) g_wifiLevelIndex = 1;    // Weak
      else g_wifiLevelIndex = 0;                     // No/very poor signal
    } else {
      g_wifiLevelIndex = 0; // Not connected
    }
  now = millis();
  // Always keep trying NTP if time is not valid
  if (!g_timeValid) {
    // wifiConnectTask handles reconnection in background

    // Try recover once per second (don’t spam)
    static uint32_t lastTry = 0;
    if (now - lastTry > 1000) {
      lastTry = now;
      tryAcquireTimeFromNTP();
    }

    // show error screen but DO NOT return forever without retry
    g_timeString = "--:--";
    u8g2.clearBuffer();
    u8g2.setFont(u8g2_font_10x20_tf);
    u8g2.setCursor(0, 24);
    u8g2.print("TIME WAIT");
    u8g2.setFont(u8g2_font_7x13B_tf);
    u8g2.setCursor(0, 44);
    u8g2.print("Connect WiFi...");
    u8g2.sendBuffer();

    delay(20);  // small yield
    return;     // OK: we return, but we retry each loop
  }
  // Battery voltage update every 500ms, moving average of last 16 readings
  static uint32_t lastBattRead = 0;
  static float vBatReadings[16] = {0};
  static int vBatIndex = 0;
  static int vBatCount = 0;
if (millis() - lastBattRead > 500) {
  lastBattRead = millis();

  float newReading = readBatteryVoltage();
  vBatReadings[vBatIndex] = newReading;
  vBatIndex = (vBatIndex + 1) % 16;
  if (vBatCount < 16) vBatCount++;

  float vBatSum = 0.0f;
  for (int i = 0; i < vBatCount; ++i) vBatSum += vBatReadings[i];
  vBat = vBatSum / vBatCount;

  checkChargerStatus();

  g_batteryLevelIndex = batteryLevelFromVoltageHyst(vBat, g_batteryLevelIndex);

  // Serial.printf("[Battery] Vbat=%.3fV, Level=%d\n", vBat, g_batteryLevelIndex);
}

  // ...existing code...




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

  // --- Sensor task metrics reporting (once per second) ---
  {
    static uint32_t lastReportMs = 0;
    if (now - lastReportMs >= 1000) {
      lastReportMs = now;
      //  Serial.printf("[sensorTask] calls/s=%lu max_dt_ms=%lu last_dt_ms=%lu deadline_miss=%lu\n",
      //               (unsigned long)g_sensorCallsPerSec,
      //               (unsigned long)g_sensorMaxDtMs,
      //               (unsigned long)g_sensorLastDtMs,
      //               (unsigned long)g_sensorDeadlineMisses);
      // reset rolling metrics window
      g_sensorCallsPerSec = 0;
      g_sensorMaxDtMs = 0;
    }
  }

  

  // WiFi check every 10s
  // if (now - lastWiFiCheck > 10000) {
  //   lastWiFiCheck = now;
  //   checkWiFi();
  // }
  // timeManager.update();



  // --- Normal operation: valid time source ---
  // Get IST time for meal logic and display

  time_t epochLocal = epoch ;//+ IST_OFFSET_SECONDS;
  gmtime_r(&epochLocal, &t);
  char dateStr[11];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &t);
 
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
  // readSensors(); // Moved to sensorTask for deterministic timing
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
    pinMode(CHARGER_DETECT_PIN, INPUT);
    // Charger status check every 2 seconds

  // pinMode(S1_D0_PIN, INPUT);
  // pinMode(S2_D0_PIN, INPUT);
  // pinMode(S3_A0_PIN, INPUT); // NEW: Sensor 3 analog pin
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
  static uint32_t lastCallMs = 0;
  uint32_t now = millis();
  // if (lastCallMs != 0) {
  //    Serial.print(">");
  //   Serial.print("T:");
  //   Serial.println(now - lastCallMs);
  // }
  // lastCallMs = now;

  // Sample analog IR only every IR_SAMPLE_INTERVAL_MS
  if (now - g_lastIrSampleMs >= IR_SAMPLE_INTERVAL_MS) {
    g_lastIrSampleMs = now;

    // Analog reads
    g_sensor1Analog = analogRead(S1_A0_PIN);
    g_sensor2Analog = analogRead(S2_A0_PIN);
    g_sensor3Analog = analogRead(S3_A0_PIN); // NEW: Sensor 3

    // Optional digital reads (for debug only, NOT used for counting)
    // g_sensor1Digital = digitalRead(S1_D0_PIN);
    // g_sensor2Digital = digitalRead(S2_D0_PIN);
    // No digital for sensor 3

    // ----- Schmitt trigger & event detection for Sensor 1 -----
    bool s1Event = false;
    if (g_s1HighRegion) {
      if (g_sensor1Analog <= IR_LTH) {
        s1Event = true;
        g_s1HighRegion = false;
      }
    } else {
      if (g_sensor1Analog >= IR_HTH) {
        g_s1HighRegion = true;
      }
    }

    // ----- Schmitt trigger & event detection for Sensor 2 -----
    bool s2Event = false;
    if (g_s2HighRegion) {
      if (g_sensor2Analog <= IR_LTH) {
        s2Event = true;
        g_s2HighRegion = false;
      }
    } else {
      if (g_sensor2Analog >= IR_HTH) {
        g_s2HighRegion = true;
      }
    }

    // ----- Schmitt trigger & event detection for Sensor 3 (NEW) -----
    bool s3Event = false;
    if (g_s3HighRegion) {
      if (g_sensor3Analog <= IR_LTH) {
        s3Event = true;
        g_s3HighRegion = false;
      }
    } else {
      if (g_sensor3Analog >= IR_HTH) {
        g_s3HighRegion = true;
      }
    }

    // OR logic between sensors with merge window (any of 3 sensors)
    bool tokenEvent = s1Event || s2Event || s3Event;
    if (tokenEvent) {
      if (now - g_lastTokenEventMs >= TOKEN_MERGE_WINDOW_MS) {
        g_lastTokenEventMs = now;

        // Check bundle timeout: if set and expired, ignore bundle
        if (g_bundleAdd > 0 && (millis() - g_bundleSetMs > 5000)) {
          g_bundleAdd = 0;
        }
        int add = (g_bundleAdd > 0) ? g_bundleAdd : 1;
        g_tokenCount += add;
        if (g_bundleAdd > 0) {
          g_bundleAdd = 0; // one-shot
        }

        if (g_tokenCount > 9999) {
          g_tokenCount = 9999;
        }
      } else {
        Serial.println("Token event merged (same token across sensors)");
      }
    }

    // Serial debug output (for plotting / diagnostics)
  //   Serial.print(">");
  //   Serial.print("S1A:");
  //   Serial.print(g_sensor1Analog);

  //   Serial.print(",S2A:");
  //   Serial.print(g_sensor2Analog);

  //   Serial.print(",S3A:");
  //   Serial.println(g_sensor3Analog);

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
  uint32_t nowMs = millis();

  // Poll current raw states (active LOW)
  bool k1 = (digitalRead(KEY1_PIN) == LOW);
  bool k2 = (digitalRead(KEY2_PIN) == LOW);
  bool k3 = (digitalRead(KEY3_PIN) == LOW);
  bool k4 = (digitalRead(KEY4_PIN) == LOW);

  // Debounced press/release tracking
  static bool k1Down = false, k2Down = false, k3Down = false, k4Down = false;
  static uint32_t k1DownMs = 0, k2DownMs = 0, k3DownMs = 0, k4DownMs = 0;

  // Combo tracking for 1 + 4
  static bool comboActive = false;
  static uint32_t comboStartMs = 0;
  static bool comboResetDone = false;
  static bool ignoreSinglesAfterCombo = false;

  // Combo tracking for 2 + 3 (bundle lock/unlock)
  static bool bundleLockComboDone = false;

  // Key 4 long-press tracking for WiFi portal (5 seconds)
  static bool k4PortalTriggered = false;

  // --- Debounced PRESS events ---
  if (k1 && !k1Down && (nowMs - g_key1LastPressMs >= BUTTON_DEBOUNCE_MS)) {
    k1Down = true;
    k1DownMs = nowMs;
    g_key1LastPressMs = nowMs;
  }
  if (k2 && !k2Down && (nowMs - g_key2LastPressMs >= BUTTON_DEBOUNCE_MS)) {
    k2Down = true;
    k2DownMs = nowMs;
    g_key2LastPressMs = nowMs;
  }
  if (k3 && !k3Down && (nowMs - g_key3LastPressMs >= BUTTON_DEBOUNCE_MS)) {
    k3Down = true;
    k3DownMs = nowMs;
    g_key3LastPressMs = nowMs;
  }
  if (k4 && !k4Down && (nowMs - g_key4LastPressMs >= BUTTON_DEBOUNCE_MS)) {
    k4Down = true;
    k4DownMs = nowMs;
    g_key4LastPressMs = nowMs;
    k4PortalTriggered = false;  // Reset portal trigger on new press
  }

  // --- Key 4 long-press (5s) for WiFi portal ---
  if (k4Down && !k1Down && !comboActive && !g_portalActive) {
    if (!k4PortalTriggered && (nowMs - k4DownMs >= 5000)) {
      k4PortalTriggered = true;
      g_lastKeyPressed = "WiFi Portal";
      Serial.println("[Keypad] Key 4 held 5s -> Starting WiFi Portal");
      startWiFiPortal();
    }
  }

  // --- Combo handling (Keys 1 + 4) ---
  bool bothDown = k1Down && k4Down;
  if (bothDown) {
    if (!comboActive) {
      comboActive = true;
      ignoreSinglesAfterCombo = true; // suppress single actions on release
      comboStartMs = (k1DownMs > k4DownMs) ? k1DownMs : k4DownMs;
      comboResetDone = false;
      Serial.println("[Keypad] Combo 1+4 started");
    }
    if (!comboResetDone && (nowMs - comboStartMs >= COMBO_RESET_HOLD_MS)) {
      g_tokenCount = 0;
      g_lastKeyPressed = "Combo 1+4 Reset";
      comboResetDone = true;
      Serial.println("Keys 1+4 held 1s -> Counter RESET");
    }
  }

  // --- Combo handling (Keys 2 + 3) for bundle lock/unlock ---
  bool bundleLockBothDown = k2Down && k3Down;
  if (bundleLockBothDown) {
    if (!g_bundleLockComboActive) {
      g_bundleLockComboActive = true;
      g_bundleLockComboStartMs = (k2DownMs > k3DownMs) ? k2DownMs : k3DownMs;
      bundleLockComboDone = false;
      Serial.println("[Keypad] Combo 2+3 started");
    }
    if (!bundleLockComboDone && (nowMs - g_bundleLockComboStartMs >= 3000)) {
      g_bundleLocked = !g_bundleLocked;
      bundleLockComboDone = true;
      g_lastKeyPressed = g_bundleLocked ? "Bundle LOCKED" : "Bundle UNLOCKED";
      Serial.printf("Keys 2+3 held 3s -> Bundle %s\n", g_bundleLocked ? "LOCKED" : "UNLOCKED");
    }
  }

  // --- Debounced RELEASE events ---
  // If combo ended (both not down), clear combo-related flags for 2+3
  if (!k2Down && !k3Down) {
    if (g_bundleLockComboActive || bundleLockComboDone) {
      g_bundleLockComboActive = false;
      bundleLockComboDone = false;
      g_bundleLockComboStartMs = 0;
    }
  }
  bool k1Released = (!k1 && k1Down && (nowMs - g_key1LastPressMs >= BUTTON_DEBOUNCE_MS));
  bool k2Released = (!k2 && k2Down && (nowMs - g_key2LastPressMs >= BUTTON_DEBOUNCE_MS));
  bool k3Released = (!k3 && k3Down && (nowMs - g_key3LastPressMs >= BUTTON_DEBOUNCE_MS));
  bool k4Released = (!k4 && k4Down && (nowMs - g_key4LastPressMs >= BUTTON_DEBOUNCE_MS));


  if (k1Released) {
    k1Down = false;
    g_key1LastPressMs = nowMs;
    if (!ignoreSinglesAfterCombo && !g_bundleLocked) {
      g_bundleAdd = 10; // single press Key 1 -> bundle +10
      g_bundleSetMs = nowMs;
      g_lastKeyPressed = "Key 1 Bundle +10";
      Serial.println("Key 1 single -> Next token = +10");
    } else if (g_bundleLocked) {
      g_lastKeyPressed = "Bundle LOCKED";
      Serial.println("Key 1 ignored: bundle locked");
    }
  }

  if (k2Released) {
    k2Down = false;
    g_key2LastPressMs = nowMs;
    if (!g_bundleLocked) {
      g_bundleAdd = 20; // single press Key 2 -> bundle +20
      g_bundleSetMs = nowMs;
      g_lastKeyPressed = "Key 2 Bundle +20";
      Serial.println("Key 2 single -> Next token = +20");
    } else {
      g_lastKeyPressed = "Bundle LOCKED";
      Serial.println("Key 2 ignored: bundle locked");
    }
  }

  if (k3Released) {
    k3Down = false;
    g_key3LastPressMs = nowMs;
    if (!g_bundleLocked) {
      g_bundleAdd = 30; // single press Key 3 -> bundle +30
      g_bundleSetMs = nowMs;
      g_lastKeyPressed = "Key 3 Bundle +30";
      Serial.println("Key 3 single -> Next token = +30");
    } else {
      g_lastKeyPressed = "Bundle LOCKED";
      Serial.println("Key 3 ignored: bundle locked");
    }
  }

  if (k4Released) {
    k4Down = false;
    g_key4LastPressMs = nowMs;
    if (!ignoreSinglesAfterCombo && !k4PortalTriggered) {
      // Short press - reserved for future use
      g_lastKeyPressed = "Key 4";
      Serial.println("Key 4 short press");
    }
    k4PortalTriggered = false;  // Reset for next press
  }

  // If combo ended (both not down), clear combo-related flags
  if (!k1Down && !k4Down) {
    if (comboActive || ignoreSinglesAfterCombo || comboResetDone) {
      comboActive = false;
      ignoreSinglesAfterCombo = false;
      comboResetDone = false;
      comboStartMs = 0;
    }
  }
}

// =============================
// Button Press Handlers
// =============================

// Key 1 → Bundle +10 (handled on release in handleKeypad)
void onKey1Pressed() {
  g_bundleAdd = 10;
  g_lastKeyPressed = "Key 1 Bundle +10";
  Serial.println("Key 1 pressed -> Next token = +10 bundle");
}

// Key 2 → Bundle +20
void onKey2Pressed() {
  g_bundleAdd = 20;
  g_lastKeyPressed = "Key 2 Bundle +20";
  Serial.println("Key 2 pressed -> Next token = +20 bundle");
}

// Key 3 → Bundle +30
void onKey3Pressed() {
  g_bundleAdd = 30;
  g_lastKeyPressed = "Key 3 Bundle +30";
  Serial.println("Key 3 pressed -> Next token = +30 bundle");
}

// Key 4 → Reserved (single press)
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
//  1..4 -> that many bars
// When portal is active, shows hotspot symbol (inverted solid triangle)
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

  // --- Hotspot/AP mode: inverted solid triangle ---
  if (g_portalActive) {
    uint8_t cx = wifiX + wifiW / 2;
    uint8_t topY = wifiY + 1;
    uint8_t bottomY = wifiY + wifiH - 2;
    uint8_t halfWidth = 5;
    
    // Draw filled inverted triangle (point at bottom, flat at top)
    // Using horizontal lines to fill
    for (int row = 0; row <= (bottomY - topY); row++) {
      int width = halfWidth - (row * halfWidth / (bottomY - topY));
      if (width < 1) width = 1;
      u8g2.drawHLine(cx - width, topY + row, width * 2 + 1);
    }
    return;
  }

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
      // Show meal indicator at extreme left (small font)
      char mealChar = ' ';
      if (currentMeal == BREAKFAST) mealChar = 'B';
      else if (currentMeal == LUNCH) mealChar = 'L';
      else if (currentMeal == DINNER) mealChar = 'D';
      u8g2.setFont(u8g2_font_5x8_mf);
      u8g2.setCursor(0, 36); // y=10, top left, small font
      if (mealChar != ' ') u8g2.print(mealChar);
    // Show bundle mode (x10/x20/x30) at left-middle if active
    if (g_bundleAdd > 0) {
      static uint32_t lastBlinkMs = 0;
      static bool blinkOn = true;
      uint32_t nowMs = millis();
      if (nowMs - lastBlinkMs >= 500) {
        blinkOn = !blinkOn;
        lastBlinkMs = nowMs;
      }
      if (blinkOn) {
        char bundleStr[6];
        snprintf(bundleStr, sizeof(bundleStr), "+%d", g_bundleAdd);
        u8g2.setFont(u8g2_font_7x13_tf); // same as time, non-bold
        int16_t bundleY = 36;
        int16_t bundleWidth = u8g2.getStrWidth(bundleStr);
        int16_t bundleX = 128 - bundleWidth - 1; // 1px margin from right
        if (bundleX < 0) bundleX = 0; // safety: never negative
        u8g2.setCursor(bundleX, bundleY);
        u8g2.print(bundleStr);
      }
    }

    // Draw bundle unlock symbol 'O' at middle extreme right, shifted up by 7 rows
    int16_t symbolW = 10, symbolH = 10;
    int16_t symbolX = 128 - symbolW - 1; // 1px from right
    int16_t symbolY = 28 - 5; // shift up by 7 rows
    if (!g_bundleLocked) {
      u8g2.setFont(u8g2_font_10x20_tf);
      u8g2.setCursor(symbolX, symbolY);
      u8g2.print("o");
    }
  const int16_t x0 = 0;
  int16_t y = 14;

  // Dynamic header: D/M/T based on deviceId
  char titleChar = 'D';
  if (strcmp(deviceId, "uno_2") == 0) titleChar = 'T';
  else if (strcmp(deviceId, "uno_3") == 0) titleChar = 'M';
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.setCursor(x0, y);
  u8g2.print(titleChar);

  // Battery blink logic: blink when charging
  static uint32_t lastBattBlinkMs = 0;
  static bool battBlinkOn = true;
  if (chargerState) {
    uint32_t nowMs = millis();
    if (nowMs - lastBattBlinkMs >= 500) {
      battBlinkOn = !battBlinkOn;
      lastBattBlinkMs = nowMs;
    }
    if (battBlinkOn) {
      drawBattery(g_batteryLevelIndex);
    }
    // else: skip drawing battery (invisible)
  } else {
    // Not charging: always show battery, reset blink state
    battBlinkOn = true;
    lastBattBlinkMs = millis();
    drawBattery(g_batteryLevelIndex);
  }
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
  int16_t countY = 64 - 15; // move up to make space for small text
  u8g2.setCursor(countX, countY);
  u8g2.print(buf);

  // Small counters line just below main counter
  // Show peer counters and sum, label based on device type
  int leftCount = 0, centerCount = 0;
  const char *leftLabel = "", *centerLabel = "";
  if (strcmp(deviceId, "uno_1") == 0) { // Darshanarthi
    leftCount = (token_data2.meal == currentMeal && token_data2.token_count >= 0) ? token_data2.token_count : 0; // Tiffin
    centerCount = (token_data3.meal == currentMeal && token_data3.token_count >= 0) ? token_data3.token_count : 0; // Mahatma
    leftLabel = "T"; centerLabel = "M";
  } else if (strcmp(deviceId, "uno_2") == 0) { // Tiffin
    leftCount = (token_data2.meal == currentMeal && token_data2.token_count >= 0) ? token_data2.token_count : 0; // Darshanarthi
    centerCount = (token_data3.meal == currentMeal && token_data3.token_count >= 0) ? token_data3.token_count : 0; // Mahatma
    leftLabel = "D"; centerLabel = "M";
  } else if (strcmp(deviceId, "uno_3") == 0) { // Mahatma
    leftCount = (token_data2.meal == currentMeal && token_data2.token_count >= 0) ? token_data2.token_count : 0; // Darshanarthi
    centerCount = (token_data3.meal == currentMeal && token_data3.token_count >= 0) ? token_data3.token_count : 0; // Tiffin
    leftLabel = "D"; centerLabel = "T";
  }
  int sum = g_tokenCount + leftCount + centerCount;
  u8g2.setFont(u8g2_font_5x8_mf);
  // Left peer
  char lBuf[10];
  snprintf(lBuf, sizeof(lBuf), "%s:%d", leftLabel, leftCount);
  u8g2.setCursor(0, 64 - 2);
  u8g2.print(lBuf);
  // Center peer
  char cBuf[10];
  snprintf(cBuf, sizeof(cBuf), "%s:%d", centerLabel, centerCount);
  int16_t cWidth = u8g2.getStrWidth(cBuf);
  int16_t cX = (128 - cWidth) / 2;
  u8g2.setCursor(cX, 64 - 2);
  u8g2.print(cBuf);
  // Σ:sum right
  char sBuf[12];
  snprintf(sBuf, sizeof(sBuf), "\xE2\x88\x91:%d", sum); // Unicode Sigma
  int16_t sWidth = u8g2.getStrWidth(sBuf);
  int16_t sX = 128 - sWidth;
  u8g2.setCursor(sX, 64 - 2);
  u8g2.print(sBuf);
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
  return dt.unixtime() > 1700000000; // ~Nov 2023
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

// =============================
// Simplified WiFi Functions
// =============================

// Load custom credentials from Preferences
void loadCustomCredentials() {
  Preferences wifiPrefs;
  wifiPrefs.begin("wificfg", true);  // read-only
  String ssid = wifiPrefs.getString("ssid", "");
  String pass = wifiPrefs.getString("pass", "");
  wifiPrefs.end();
  
  strncpy(g_customSSID, ssid.c_str(), sizeof(g_customSSID) - 1);
  g_customSSID[sizeof(g_customSSID) - 1] = '\0';
  strncpy(g_customPass, pass.c_str(), sizeof(g_customPass) - 1);
  g_customPass[sizeof(g_customPass) - 1] = '\0';
  
  if (g_customSSID[0]) {
    Serial.printf("[WiFi] Loaded custom SSID: %s\n", g_customSSID);
  }
}

// Save custom credentials to Preferences
void saveCustomCredentials(const char* ssid, const char* pass) {
  Preferences wifiPrefs;
  wifiPrefs.begin("wificfg", false);  // read-write
  wifiPrefs.putString("ssid", ssid);
  wifiPrefs.putString("pass", pass);
  wifiPrefs.end();
  
  strncpy(g_customSSID, ssid, sizeof(g_customSSID) - 1);
  g_customSSID[sizeof(g_customSSID) - 1] = '\0';
  strncpy(g_customPass, pass, sizeof(g_customPass) - 1);
  g_customPass[sizeof(g_customPass) - 1] = '\0';
  
  Serial.printf("[WiFi] Saved custom SSID: %s\n", ssid);
}

// Generate dynamic HTML page for WiFi configuration portal
String generatePortalHTML() {
  String mac = WiFi.macAddress();
  String chipId = String((uint32_t)(ESP.getEfuseMac() >> 32), HEX) + String((uint32_t)ESP.getEfuseMac(), HEX);
  chipId.toUpperCase();
  uint32_t freeHeap = ESP.getFreeHeap();
  uint32_t flashSize = ESP.getFlashChipSize() / 1024 / 1024;
  String currentSSID = g_customSSID[0] ? String(g_customSSID) : "(not set)";
  
  String html = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>WiFi Setup</title>
  <style>
    body { font-family: Arial; margin: 20px; background: #f0f0f0; }
    .container { max-width: 400px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); }
    h1 { color: #333; text-align: center; margin-bottom: 5px; }
    h3 { color: #666; text-align: center; margin-top: 0; font-weight: normal; }
    label { display: block; margin-top: 10px; color: #555; }
    input[type=text], input[type=password] { width: 100%; padding: 12px; margin: 5px 0; box-sizing: border-box; border: 1px solid #ccc; border-radius: 4px; font-size: 16px; }
    input[type=submit] { width: 100%; background-color: #4CAF50; color: white; padding: 14px; margin: 15px 0 8px 0; border: none; border-radius: 4px; cursor: pointer; font-size: 16px; }
    input[type=submit]:hover { background-color: #45a049; }
    .show-pass { display: flex; align-items: center; margin: 5px 0; }
    .show-pass input { width: auto; margin-right: 8px; }
    .show-pass label { margin: 0; color: #666; font-size: 14px; }
    .info-box { background: #f8f9fa; border: 1px solid #e9ecef; border-radius: 6px; padding: 12px; margin-top: 15px; }
    .info-box h4 { margin: 0 0 10px 0; color: #495057; font-size: 14px; border-bottom: 1px solid #dee2e6; padding-bottom: 5px; }
    .info-row { display: flex; justify-content: space-between; margin: 5px 0; font-size: 12px; }
    .info-label { color: #6c757d; }
    .info-value { color: #212529; font-family: monospace; word-break: break-all; }
    .footer { color: #999; font-size: 11px; text-align: center; margin-top: 15px; }
  </style>
</head>
<body>
  <div class="container">
    <h1>WiFi Setup</h1>
    <h3>)rawliteral";
  
  html += String(deviceId);
  html += R"rawliteral(</h3>
    <form action="/save" method="POST">
      <label>WiFi Network (SSID):</label>
      <input type="text" name="ssid" id="ssid" placeholder="Enter WiFi name" required>
      <label>Password:</label>
      <input type="password" name="pass" id="pass" placeholder="Enter WiFi password">
      <div class="show-pass">
        <input type="checkbox" id="showPass" onclick="togglePassword()">
        <label for="showPass">Show password</label>
      </div>
      <input type="submit" value="Save & Connect">
    </form>
    
    <div class="info-box">
      <h4>Device Information</h4>
      <div class="info-row">
        <span class="info-label">Device ID:</span>
        <span class="info-value">)rawliteral";
  html += String(deviceId);
  html += R"rawliteral(</span>
      </div>
      <div class="info-row">
        <span class="info-label">MAC Address:</span>
        <span class="info-value">)rawliteral";
  html += mac;
  html += R"rawliteral(</span>
      </div>
      <div class="info-row">
        <span class="info-label">Chip ID:</span>
        <span class="info-value">)rawliteral";
  html += chipId;
  html += R"rawliteral(</span>
      </div>
      <div class="info-row">
        <span class="info-label">Flash Size:</span>
        <span class="info-value">)rawliteral";
  html += String(flashSize) + " MB";
  html += R"rawliteral(</span>
      </div>
      <div class="info-row">
        <span class="info-label">Free Heap:</span>
        <span class="info-value">)rawliteral";
  html += String(freeHeap / 1024) + " KB";
  html += R"rawliteral(</span>
      </div>
      <div class="info-row">
        <span class="info-label">Saved SSID:</span>
        <span class="info-value">)rawliteral";
  html += currentSSID;
  html += R"rawliteral(</span>
      </div>
      <div class="info-row">
        <span class="info-label">AP IP:</span>
        <span class="info-value">192.168.4.1</span>
      </div>
    </div>
    
    <p class="footer">Hold Button 4 for 5s to reopen this portal</p>
  </div>
  
  <script>
    function togglePassword() {
      var passField = document.getElementById('pass');
      passField.type = passField.type === 'password' ? 'text' : 'password';
    }
  </script>
</body>
</html>
)rawliteral";
  
  return html;
}

static const char PORTAL_SUCCESS_HTML[] PROGMEM = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>WiFi Saved</title>
  <style>
    body { font-family: Arial; margin: 20px; background: #f0f0f0; }
    .container { max-width: 400px; margin: 0 auto; background: white; padding: 20px; border-radius: 10px; box-shadow: 0 2px 10px rgba(0,0,0,0.1); text-align: center; }
    h1 { color: #4CAF50; }
    p { color: #333; }
  </style>
</head>
<body>
  <div class="container">
    <h1>&#10004; Saved!</h1>
    <p>WiFi credentials saved successfully.</p>
    <p>Device will now try to connect...</p>
    <p>Hotspot will close in a few seconds.</p>
  </div>
</body>
</html>
)rawliteral";

// Handle portal root page
void handlePortalRoot() {
  if (g_portalServer) {
    String html = generatePortalHTML();
    g_portalServer->send(200, "text/html", html);
  }
}

// Handle save credentials
void handlePortalSave() {
  if (!g_portalServer) return;
  
  String ssid = g_portalServer->arg("ssid");
  String pass = g_portalServer->arg("pass");
  
  if (ssid.length() > 0) {
    saveCustomCredentials(ssid.c_str(), pass.c_str());
    g_portalServer->send(200, "text/html", PORTAL_SUCCESS_HTML);
    Serial.println("[Portal] Credentials saved, will stop portal soon...");
    
    // Set flag to stop portal - actual stop will happen in processPortal()
    // This avoids deleting the server while inside a handler
    g_portalStopRequested = true;
  } else {
    g_portalServer->send(400, "text/plain", "SSID required");
  }
}

// Start WiFi configuration portal (AP mode only)
void startWiFiPortal() {
  if (g_portalActive) {
    Serial.println("[Portal] Already active");
    return;
  }
  
  Serial.println("[Portal] Starting WiFi configuration portal...");
  
  // Reset stop request flag
  g_portalStopRequested = false;
  
  // Stop any existing WiFi connection
  WiFi.disconnect(true);
  delay(100);
  
  // Start AP mode only (hotspot)
  String apName = String(deviceId) + "_SETUP";
  WiFi.mode(WIFI_AP);
  WiFi.softAP(apName.c_str());
  
  IPAddress apIP = WiFi.softAPIP();
  Serial.printf("[Portal] AP started: %s, IP: %s\n", apName.c_str(), apIP.toString().c_str());
  
  // Create DNS server for captive portal
  g_dnsServer = new DNSServer();
  g_dnsServer->start(53, "*", apIP);
  
  // Create web server
  g_portalServer = new WebServer(80);
  g_portalServer->on("/", HTTP_GET, handlePortalRoot);
  g_portalServer->on("/save", HTTP_POST, handlePortalSave);
  g_portalServer->onNotFound(handlePortalRoot);  // Redirect all to root
  g_portalServer->begin();
  
  g_portalActive = true;
  g_portalStartMs = millis();
  
  Serial.println("[Portal] Web server started on port 80");
}

// Stop WiFi portal and try connecting to networks
void stopWiFiPortal() {
  if (!g_portalActive) return;
  
  Serial.println("[Portal] Stopping portal...");
  
  if (g_portalServer) {
    g_portalServer->stop();
    delete g_portalServer;
    g_portalServer = nullptr;
  }
  
  if (g_dnsServer) {
    g_dnsServer->stop();
    delete g_dnsServer;
    g_dnsServer = nullptr;
  }
  
  WiFi.softAPdisconnect(true);
  WiFi.mode(WIFI_STA);
  
  g_portalActive = false;
  
  Serial.println("[Portal] Portal stopped, switching to STA mode");
  
  // Try to connect to available networks
  tryConnectToNetworks();
}

// Process portal (call from main loop when portal is active)
void processPortal() {
  if (!g_portalActive) return;
  
  // Check if stop was requested (from save handler)
  if (g_portalStopRequested) {
    static uint32_t stopRequestedMs = 0;
    if (stopRequestedMs == 0) {
      stopRequestedMs = millis();  // Record when stop was requested
    }
    // Wait 2 seconds for the response to be sent to client before stopping
    if (millis() - stopRequestedMs > 2000) {
      Serial.println("[Portal] Stop requested, stopping now");
      stopRequestedMs = 0;  // Reset for next time
      g_portalStopRequested = false;
      stopWiFiPortal();
      return;
    }
  }
  
  if (g_dnsServer) g_dnsServer->processNextRequest();
  if (g_portalServer) g_portalServer->handleClient();
  
  // Timeout check
  if (millis() - g_portalStartMs > PORTAL_TIMEOUT_MS) {
    Serial.println("[Portal] Timeout, stopping portal");
    stopWiFiPortal();
  }
}

// Try to connect to one of the available networks (scan and connect)
bool tryConnectToNetworks() {
  Serial.println("[WiFi] Scanning for networks...");
  
  WiFi.mode(WIFI_STA);
  WiFi.disconnect();
  delay(100);
  
  int n = WiFi.scanNetworks();
  Serial.printf("[WiFi] Found %d networks\n", n);
  
  // Check which of our 3 SSIDs are available
  bool foundStatic1 = false, foundStatic2 = false, foundCustom = false;
  
  for (int i = 0; i < n; i++) {
    String ssid = WiFi.SSID(i);
    if (ssid == STATIC_SSID1) foundStatic1 = true;
    if (ssid == STATIC_SSID2) foundStatic2 = true;
    if (g_customSSID[0] && ssid == g_customSSID) foundCustom = true;
  }
  
  WiFi.scanDelete();
  
  // Try to connect in order: custom first (user preference), then static
  const char* ssidToTry = nullptr;
  const char* passToTry = nullptr;
  
  if (foundCustom && g_customSSID[0]) {
    ssidToTry = g_customSSID;
    passToTry = g_customPass;
    Serial.printf("[WiFi] Trying custom SSID: %s\n", ssidToTry);
  } else if (foundStatic1) {
    ssidToTry = STATIC_SSID1;
    passToTry = STATIC_PASS1;
    Serial.printf("[WiFi] Trying static SSID1: %s\n", ssidToTry);
  } else if (foundStatic2) {
    ssidToTry = STATIC_SSID2;
    passToTry = STATIC_PASS2;
    Serial.printf("[WiFi] Trying static SSID2: %s\n", ssidToTry);
  }
  
  if (ssidToTry) {
    WiFi.begin(ssidToTry, passToTry);
    
    // Wait for connection (max 10 seconds)
    uint32_t startMs = millis();
    while (WiFi.status() != WL_CONNECTED && (millis() - startMs) < 10000) {
      delay(250);
    }
    
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf("[WiFi] Connected to %s, IP: %s\n", ssidToTry, WiFi.localIP().toString().c_str());
      return true;
    } else {
      Serial.printf("[WiFi] Failed to connect to %s\n", ssidToTry);
    }
  } else {
    Serial.println("[WiFi] No known networks found");
  }
  
  return false;
}

// Background WiFi connection task (simple reconnect logic)
void wifiConnectTask(void *pv) {
  WiFi.mode(WIFI_STA);
  WiFi.setAutoReconnect(true);
  WiFi.setSleep(false);
  
  uint32_t lastConnectTry = 0;
  uint8_t networkIndex = 0;  // 0 = custom, 1 = static1, 2 = static2
  
  // Initial connection attempt
  tryConnectToNetworks();
  
  for (;;) {
    // Process portal if active
    if (g_portalActive) {
      processPortal();
      vTaskDelay(pdMS_TO_TICKS(10));
      continue;
    }
    
    bool connected = (WiFi.status() == WL_CONNECTED);
    
    if (connected) {
      // Update WiFi signal strength indicator
      long rssi = WiFi.RSSI();
      if (rssi >= -55) g_wifiLevelIndex = 4;
      else if (rssi >= -65) g_wifiLevelIndex = 3;
      else if (rssi >= -75) g_wifiLevelIndex = 2;
      else if (rssi >= -85) g_wifiLevelIndex = 1;
      else g_wifiLevelIndex = 0;
      
      vTaskDelay(pdMS_TO_TICKS(500));
      continue;
    }
    
    // Not connected
    g_wifiLevelIndex = 0;
    
    // Try reconnecting every 10 seconds
    if (millis() - lastConnectTry > 10000) {
      lastConnectTry = millis();
      
      // Cycle through available networks
      const char* ssid = nullptr;
      const char* pass = nullptr;
      
      switch (networkIndex) {
        case 0:
          if (g_customSSID[0]) {
            ssid = g_customSSID;
            pass = g_customPass;
          }
          break;
        case 1:
          ssid = STATIC_SSID1;
          pass = STATIC_PASS1;
          break;
        case 2:
          ssid = STATIC_SSID2;
          pass = STATIC_PASS2;
          break;
      }
      
      networkIndex = (networkIndex + 1) % 3;
      
      if (ssid && ssid[0]) {
        Serial.printf("[WiFi] Trying: %s\n", ssid);
        WiFi.disconnect();
        WiFi.begin(ssid, pass);
      }
    }
    
    vTaskDelay(pdMS_TO_TICKS(250));
  }
}


static bool tryAcquireTimeFromNTP() {
  if (WiFi.status() != WL_CONNECTED) return false;

  // SNTP starts in background; calling again is OK
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");

  time_t e = time(nullptr);
  if (e < 1000000000) return false; // not valid yet

  g_timeSource = TIME_NTP;
  g_timeValid  = true;
  strcpy(g_timeErrorMsg, "");

  // If RTC invalid, set it now
  setSystemTimeFromEpoch(e);
  rtc.adjust(DateTime(e));   // you can always set it here
  Serial.println("[TIME] Recovered from NTP in loop.");
  return true;
}


inline void setSystemTimeFromEpoch(time_t e) {
  struct timeval tv;
  tv.tv_sec = e;
  tv.tv_usec = 0;
  settimeofday(&tv, nullptr);
}

// NEW: Dedicated sensor sampling task running at fixed period
void sensorTask(void *pv) {
  const TickType_t periodTicks = pdMS_TO_TICKS(SENSOR_TASK_PERIOD_MS);
  TickType_t lastWake = xTaskGetTickCount();
  for (;;) {
    // Timing metrics
    uint32_t msNow;
    msNow = millis();
    static uint32_t prevMs = 0;
    if (prevMs != 0) {
      uint32_t dt = msNow - prevMs;
      g_sensorLastDtMs = dt;
      if (dt > g_sensorMaxDtMs) g_sensorMaxDtMs = dt;
      // Consider deadline miss if dt exceeds period + small margin (1ms)
      if (dt > (SENSOR_TASK_PERIOD_MS + 1)) {
        g_sensorDeadlineMisses++;

      }
    }
    prevMs = msNow;

    g_sensorCallsTotal++;
    g_sensorCallsPerSec++;

    readSensors();
    vTaskDelayUntil(&lastWake, periodTicks);
  }
}

// // NEW: Fetch peer data if needed (auto-fetch logic moved from loop())
// void fetchPeerDataIfNeeded() {
//     // Update currentMeal based on meal window logic
//     time_t epoch = time(nullptr) + 19800; // IST offset +5:30
//     struct tm t;
//     gmtime_r(&epoch, &t);
//     int hour = t.tm_hour;
//     mealType newMeal = NONE;
//     if (hour >= BFL && hour < BFH) newMeal = BREAKFAST;
//     else if (hour >= LFL && hour <= LFH) newMeal = LUNCH;
//     else if (hour >= DFL && hour <= DFH) newMeal = DINNER;
//     else newMeal = NONE;
//     currentMeal = newMeal;

//     if (strcmp(deviceId, "uno_1") == 0 && WiFi.status() == WL_CONNECTED && currentMeal != NONE) {
//         static uint32_t lastPeerFetch = 0;
//         uint32_t nowFetch = millis();
//         if (nowFetch - lastPeerFetch > 10000) { // fetch every 10s
//             lastPeerFetch = nowFetch;
//             char dateStr[11];
//             strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &t);
//             auto fetchPeer = [&dateStr](const char* peerId, TokenData* peerData, const char* apikey, mealType meal) {
//                 HTTPClient http;
//                 String url = String(postURL) + "?sensor_id=eq." + peerId + "&date=eq." + peerData->date;
//                 http.begin(url);
//                 http.addHeader("apikey", apikey);
//                 int code = http.GET();
//                 Serial.printf("[PeerFetch] GET %s -> code %d\n", url.c_str(), code);
//                 if (code == 200) {
//                     String payload = http.getString();
//                     JsonDocument doc;
//                     DeserializationError err = deserializeJson(doc, payload);
//                     if (!err && doc.is<JsonArray>() && doc.size() > 0) {
//                         JsonObject obj = doc[0];
//                         int mealCount = 0;
//                         if (meal == BREAKFAST) mealCount = obj["breakfast"] | 0;
//                         else if (meal == LUNCH) mealCount = obj["lunch"] | 0;
//                         else if (meal == DINNER) mealCount = obj["dinner"] | 0;
//                         peerData->token_count = mealCount;
//                         strncpy(peerData->date, obj["date"] | dateStr, sizeof(peerData->date));
//                         peerData->date[10] = '\0';
//                         peerData->meal = meal;
//                         Serial.printf("[PeerFetch] %s: meal=%d, count=%d, date=%s\n", peerId, (int)meal, mealCount, peerData->date);
//                     } else {
//                         Serial.printf("[PeerFetch] %s: JSON parse error or empty array\n", peerId);
//                     }
//                 } else {
//                     Serial.printf("[PeerFetch] %s: HTTP GET failed\n", peerId);
//                 }
//                 http.end();
//             };
//             strncpy(token_data2.date, dateStr, sizeof(token_data2.date));
//             token_data2.date[10] = '\0';
//             fetchPeer(deviceId2, &token_data2, apikey, currentMeal);
//             strncpy(token_data3.date, dateStr, sizeof(token_data3.date));
//             token_data3.date[10] = '\0';
//             fetchPeer(deviceId3, &token_data3, apikey, currentMeal);
//         }
//     }
// }


// --- fetchPeer: file-scope, blocking, HTTP+JSON only ---
void fetchPeer(const char* peerId, TokenData* peerData, const char* apikey, mealType meal, const char* dateStr) {
  HTTPClient http;
  String url = String(postURL) + "?sensor_id=eq." + peerId + "&date=eq." + dateStr;
  http.begin(url);
  http.addHeader("apikey", apikey);
  http.setTimeout(1500);  // 1.5 seconds
  int code = http.GET();
  Serial.printf("[PeerFetch] GET %s -> code %d\n", url.c_str(), code);
  if (code == 200) {
    String payload = http.getString();
    JsonDocument doc;
    DeserializationError err = deserializeJson(doc, payload);
    if (!err && doc.is<JsonArray>() && doc.size() > 0) {
      JsonObject obj = doc[0];
      int mealCount = 0;
      if (meal == BREAKFAST) mealCount = obj["breakfast"] | 0;
      else if (meal == LUNCH) mealCount = obj["lunch"] | 0;
      else if (meal == DINNER) mealCount = obj["dinner"] | 0;
      peerData->token_count = mealCount;
      strncpy(peerData->date, obj["date"] | dateStr, sizeof(peerData->date));
      peerData->date[10] = '\0';
      peerData->meal = meal;
      Serial.printf("[PeerFetch] %s: meal=%d, count=%d, date=%s\n", peerId, (int)meal, mealCount, peerData->date);
    } else {
      Serial.printf("[PeerFetch] %s: JSON parse error or empty array\n", peerId);
    }
  } else {
    Serial.printf("[PeerFetch] %s: HTTP GET failed\n", peerId);
  }
  http.end();
}

// --- Peer fetch FreeRTOS task ---
void peerFetchTask(void* pv) {
  PeerFetchRequest req;
  
  for (;;) {
    static uint32_t lastPrint = 0;
uint32_t now = millis();
if (now - lastPrint > 2000) {
  lastPrint = now;
  Serial.printf("peerFetchTask HW=%u words (%u bytes)\n",
                uxTaskGetStackHighWaterMark(NULL),
                uxTaskGetStackHighWaterMark(NULL) * 4);
}

    if (xQueueReceive(peerFetchQueue, &req, portMAX_DELAY) == pdTRUE) {
      fetchPeer(req.peerId, req.peerData, apikey, req.meal, req.dateStr);
    }
  }
}

// --- fetchPeerDataIfNeeded: scheduler only, no HTTP ---
void fetchPeerDataIfNeeded() {
  static uint32_t lastPeerFetch = 0;
  uint32_t nowFetch = millis();
  time_t epoch = time(nullptr) + 19800; // IST offset +5:30
  struct tm t;
  gmtime_r(&epoch, &t);
  int hour = t.tm_hour;
  mealType newMeal = NONE;
  if (hour >= BFL && hour <= BFH) newMeal = BREAKFAST;
  else if (hour >= LFL && hour <= LFH) newMeal = LUNCH;
  else if (hour >= DFL && hour <= DFH) newMeal = DINNER;
  else newMeal = NONE;
  currentMeal = newMeal;
  if (WiFi.status() == WL_CONNECTED && currentMeal != NONE) {
    if (nowFetch - lastPeerFetch > 10000) { // 10s interval
      lastPeerFetch = nowFetch;
      char dateStr[11];
      strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &t);
      PeerFetchRequest req2, req3;
      strncpy(req2.peerId, deviceId2, sizeof(req2.peerId));
      strncpy(req2.dateStr, dateStr, sizeof(req2.dateStr));
      req2.meal = currentMeal;
      req2.peerData = &token_data2;
      strncpy(req3.peerId, deviceId3, sizeof(req3.peerId));
      strncpy(req3.dateStr, dateStr, sizeof(req3.dateStr));
      req3.meal = currentMeal;
      req3.peerData = &token_data3;
      xQueueSend(peerFetchQueue, &req2, 0);
      xQueueSend(peerFetchQueue, &req3, 0);
    }
  }
}

static uint8_t batteryLevelFromVoltageHyst(float vBat, uint8_t curLevel) {
  const float H = 0.05f; // 50 mV hysteresis

  // Your thresholds (edit these as you like)
  const float T4 = 3.95f; // Level 4 boundary
  const float T3 = 3.75f; // Level 3 boundary
  const float T2 = 3.62f; // Level 2 boundary
  const float T1 = 3.35f; // Level 1 boundary
  const float T0 = 3.1f; // Level 0 / empty reference (optional)

  // curLevel is 0..4
  switch (curLevel) {
    case 4:
      if (vBat < (T4 - H)) return 3;
      return 4;

    case 3:
      if (vBat >= (T4 + H)) return 4;
      if (vBat <  (T3 - H)) return 2;
      return 3;

    case 2:
      if (vBat >= (T3 + H)) return 3;
      if (vBat <  (T2 - H)) return 1;
      return 2;

    case 1:
      if (vBat >= (T2 + H)) return 2;
      if (vBat <  (T1 - H)) return 0;
      return 1;

    case 0:
    default:
      if (vBat >= (T1 + H)) return 1;
      return 0;
  }
}
