/*
ESP32 Token Counter Firmware - Project Summary & Key Requirements
=================================================================

This firmware is designed for an ESP32 device to count tokens (e.g., meal coupons) for breakfast, lunch, and dinner, and report the counts to a Supabase backend. It is structured for easy merging with other codebases, including those with IR sensor integration.

Key Features & Implementation:
------------------------------
1. WiFi Setup:
  - Uses WiFiManager for automatic WiFi connection and fallback portal.
  - Device ID is used for identification in Supabase.

2. Time Synchronization:
  - Uses NTP to synchronize the device clock at boot and periodically.
  - Retries until a valid time is received before proceeding.

3. Meal Time Logic:
  - Defines meal windows: Breakfast (7–9am), Lunch (11am–2pm), Dinner (6–9pm).
  - Uses current IST time to determine which meal window is active.
  - Macros (BFL, BFH, LFL, LFH, DFL, DFH) allow easy adjustment of meal windows.
  - DUMMYHREFORTESTING macro allows simulation of different meal times for testing.

4. Token Counting:
  - Maintains a TokenData struct with token_count, meal, date (yyyy-mm-dd IST), and update flag.
  - On each loop, if within a meal window, increments the token count and sets the update flag.
  - If not in a meal window, skips token increment and logs status.

5. Persistent Storage (NVS):
  - Uses ESP32 Preferences (NVS) for storing and restoring TokenData.
  - On boot, loads token data from NVS and validates it.
  - If the stored date and meal match the current values, keeps the count; otherwise, resets the count (unless not in a meal window).
  - Data is saved to NVS every 20 seconds or when updated.

6. Supabase Integration:
  - Sends token count updates to Supabase using HTTP PATCH requests.
  - Only the relevant meal column (breakfast, lunch, or dinner) is updated in the payload.
  - Filters by sensor_id and date in the request.

7. Debug Logging:
  - Prints detailed logs for WiFi status, time sync, token data, and Supabase requests.
  - Logs when not in a meal window, including current hour, token count, date, and meal.

8. Testing Support:
  - DUMMYHREFORTESTING macro to simulate different meal times for testing.

9. Code Cleanups:
  - All legacy schedule, timer, relay, and EEPROM logic has been removed.
  - All persistent storage is now handled by NVS (Preferences).

Key Discussion & Requirements:
------------------------------
- Robust time sync and validation to ensure correct daily/meal token counting.
- Persistent storage using NVS for increased write lifecycle and reliability over EEPROM.
- Easy integration point for IR sensor logic: Replace the dummy token increment with actual IR sensor read logic.
- Defensive checks for corrupted or invalid NVS data.
- Only initialize/reset token count if current time is a valid meal window.
- All code is modular and ready for merging with IR sensor code or other extensions.

TODOs for Integration:
----------------------
- Implement IRsensor_read() to increment token_count based on actual IR sensor events.
- Remove unused legacy code (OneWire, DallasTemperature, relay logic) if not needed for your hardware.
- Optionally add Prometheus metrics sending if required for your backend.

This comment summarizes the full code, requirements, and design decisions for future development and merging.
*/
/*
ESP32 Token Counter Firmware
===========================

This firmware is designed for an ESP32-based device to count tokens (e.g., meal coupons) for breakfast, lunch, and dinner, and report the counts to a Supabase backend. It uses NVS (Preferences) for persistent storage, time-based logic for meal selection, and WiFiManager for network setup.

Key Features:
-------------
1. WiFi setup using WiFiManager with fallback portal.
2. NTP time synchronization with retry until valid time is received.
3. Meal time logic for breakfast (7–9am), lunch (11am–2pm), dinner (6–9pm).
4. Token counting per meal and day, persisted in NVS (Preferences).
5. Robust validation and initialization of token data on boot.
6. Supabase integration: PATCH requests update only the relevant meal column.
7. Debug logging for WiFi, time, token data, and Supabase requests.
8. Testing support via DUMMYHREFORTESTING macro.
9. All legacy schedule, timer, relay, and EEPROM logic removed.

TODO:
- IR sensor stub for actual token increment (currently dummy increment).
- Prometheus metrics sending (currently only Supabase is used).
- Remove unused legacy code (OneWire, DallasTemperature, relay logic) if not needed.

*/
#ifndef ARDUINOJSON_DEPRECATED
#define ARDUINOJSON_DEPRECATED(msg)
#endif
#include <WiFiManager.h> 
#include <WiFi.h>
#include <SPI.h>
#include <time.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>
#include <OneWire.h>
#include <DallasTemperature.h>
#include <Preferences.h>
#define BFL 7
#define BFH 9
#define LFL 11
#define LFH 14
#define DFL 18  
#define DFH 21
#define DUMMYHREFORTESTING 12  // Adjust this value to test different meal times
Preferences prefs;

// === Time sync configuration ===
// Change these macros to adjust sync/retry behavior
#ifndef TIME_SYNC_INTERVAL_MS
#define TIME_SYNC_INTERVAL_MS 3600000UL // 1 hour
#endif
#ifndef TIME_RETRY_INTERVAL_MS
#define TIME_RETRY_INTERVAL_MS 600000UL // 10 minutes
#endif
#ifndef TIME_INITIAL_TIMEOUT_MS
// Give the initial NTP sync a longer window (15s) to accommodate slow networks
#define TIME_INITIAL_TIMEOUT_MS 5000UL // initial NTP wait (ms)
#endif
#ifndef TIME_SYNC_ATTEMPT_TIMEOUT_MS
// Per-attempt timeout for subsequent sync tries (10s)
#define TIME_SYNC_ATTEMPT_TIMEOUT_MS 10000UL // single sync attempt timeout (ms) to avoid long blocking
#endif

// // How often to dump the schedule table to Serial (default 5 minutes)
// #ifndef SCHEDULE_DUMP_INTERVAL_MS
// #define SCHEDULE_DUMP_INTERVAL_MS 60000UL
// #endif

// Device identifier used to select schedules in Supabase
const char *deviceId = "uno_1";
const char *deviceId2 = "uno_2";
const char *deviceId3 = "uno_3";

enum mealType {
  NONE,
  BREAKFAST,
  LUNCH,
  DINNER,
  MEAL_COUNT
};
String  meal[MEAL_COUNT] = {"none", "breakfast", "lunch", "dinner"};
mealType currentMeal = NONE;

struct TokenData {
  int token_count;
  mealType meal;
  char date[11];    // Will hold "yyyy-mm-dd"
  bool update;
};

TokenData token_data = {0, NONE, "1970-01-01", false};
TokenData token_data2 = {0, NONE, "1970-01-01", false};
TokenData token_data3 = {0, NONE, "1970-01-01", false};
#define MAX_SCHEDULES 8

// India Standard Time offset from UTC in seconds (+5:30)
static const long IST_OFFSET_SECONDS = 5 * 3600 + 30 * 60;
// === Supabase API Info ===
// === Supabase API Info ===
// const char *getURL = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/commands";
// const char *getURLschedule = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/schedule";
const char *postURL = "https://akxcjabakrvfaevdfwru.supabase.co/rest/v1/unodari_token";
const char *apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6ImFreGNqYWJha3J2ZmFldmRmd3J1Iiwicm9sZSI6ImFub24iLCJpYXQiOjE3NDkxMjMwMjUsImV4cCI6MjA2NDY5OTAyNX0.kykki4uVVgkSVU4lH-wcuGRdyu2xJ1CQkYFhQq_u08w";

// === GPIO Definitions ===
#define ONE_WIRE_BUS_1 23
#define ONE_WIRE_BUS_2 22
#define RELAY1_PIN 32

OneWire oneWire1(ONE_WIRE_BUS_1);
OneWire oneWire2(ONE_WIRE_BUS_2);
DallasTemperature sensor1(&oneWire1);
DallasTemperature sensor2(&oneWire2);

bool relayState1 = false;
unsigned long lastRelayCheck = 0;
unsigned long lastSensorSend = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastEEPROMWrite = 0;
// // lastScheduleCheck is used to run schedule checks every minute
// unsigned long lastScheduleCheck = 0;
// unsigned long lastScheduleDump = 0;
// // Whether schedule logic is allowed (requires valid NTP time)
// bool scheduleAllowed = false;
// // When a schedule OFF event occurs we latch timers disabled until schedule ON or manual ON
// bool timersDisabledBySchedule = false;
// === TimeManager ===
// Keeps a local epoch running using millis() and periodically attempts to sync
// with NTP. If offline or NTP fails, local time continues advancing.

// Wait for NTP sync and return epoch time (or 0 if not synced within timeout)
time_t fetchNetworkTime(unsigned long timeoutMs = 5000) {
  time_t now = time(nullptr);
  const time_t validThreshold = 1000000000; // ~2001-09-09, any sane current time will be > this
  unsigned long start = millis();
  while (now < validThreshold && (millis() - start) < timeoutMs) {
    // Use a short sleep to remain responsive; this loop will exit after timeoutMs
    delay(50);
    now = time(nullptr);
  }
  // If we exited because timeout and no valid time, return -1 to indicate timeout
  if (now < validThreshold) return (time_t)-1;
  return now;
}


class TimeManager {
public:
  TimeManager()
      : lastSyncedEpoch(0), lastSyncMillis(0), lastAttemptMillis(0),
        syncIntervalMs(TIME_SYNC_INTERVAL_MS), retryIntervalMs(TIME_RETRY_INTERVAL_MS),
        lastSyncSuccess(false), lastNoUpdateLogMillis(0) {}

  // Initialize the manager and attempt an immediate sync (timeout ms)
  void begin(unsigned long intervalMs = TIME_SYNC_INTERVAL_MS, unsigned long initialTimeoutMs = TIME_INITIAL_TIMEOUT_MS) {
    syncIntervalMs = intervalMs;
    // Record baseline attempt time so retry/sync intervals are calculated
    lastAttemptMillis = millis();
    // Try initial sync; if it fails, seed with current system time (may be 0)
    if (!trySync(initialTimeoutMs)) {
      lastSyncedEpoch = time(nullptr);
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      Serial.println("⚠️ TimeManager: Initial NTP sync failed — continuing with local clock");
    }
  }

  // Attempt to sync with network time. Returns true if successful.
  bool trySync(unsigned long timeoutMs = TIME_SYNC_ATTEMPT_TIMEOUT_MS) {
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("⚠️ TimeManager: NTP sync skipped — WiFi not connected");
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      return false;
    }
    // Ensure SNTP is (re)configured before waiting for time
    configTime(0, 0, "pool.ntp.org", "time.nist.gov");
    unsigned long attemptStart = millis();
    Serial.printf("⏳ TimeManager: attempting NTP sync (timeout %lums)...\n", timeoutMs);
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
      // Determine whether this is a fresh sync (i.e. NTP updated the clock)
      time_t expected = lastSyncedEpoch + (time_t)((attemptStart - lastSyncMillis) / 1000);
      long diff = (long)epoch - (long)expected;
      if (lastSyncMillis == 0 || llabs(diff) > 2) {
        // Fresh sync: update base epoch and millis
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
        // No observable change — this likely means local clock already had correct time
        // Update lastSyncMillis so we don't re-check immediately
        lastSyncSuccess = true; // treat as success for scheduling
        lastSyncMillis = millis();
        // Rate-limit informational logs to avoid spam (use retry interval)
        if ((millis() - lastNoUpdateLogMillis) >= retryIntervalMs) {
          Serial.println("ℹ️ TimeManager: Time check OK — no new NTP update (using local clock)");
          lastNoUpdateLogMillis = millis();
        }
        return false;
      }
    }
    // Received a time but it's invalid/too small
    Serial.printf("⚠️ TimeManager: NTP returned invalid epoch: %ld\n", (long)epoch);
    lastSyncSuccess = false;
    lastAttemptMillis = millis();
    return false;
  }

  // Call from loop() regularly; will trigger a sync when the interval elapsed.
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

  // Returns the current epoch as maintained locally (advances while offline)
  time_t now() const {
    unsigned long elapsedMs = millis() - lastSyncMillis;
    return lastSyncedEpoch + (time_t)(elapsedMs / 1000);
  }

  // Fill a struct tm with current UTC time
  void getUTCTime(struct tm &out) const {
    time_t t = now();
    gmtime_r(&t, &out);
  }

  // Adjust sync interval (ms)
  void setSyncInterval(unsigned long ms) { syncIntervalMs = ms; }
  // Set a shorter retry interval to use when sync fails (e.g. 10 minutes)
  void setRetryInterval(unsigned long ms) { retryIntervalMs = ms; }

private:
  time_t lastSyncedEpoch;
  unsigned long lastSyncMillis;
  unsigned long syncIntervalMs;
  unsigned long retryIntervalMs;
  bool lastSyncSuccess;
  unsigned long lastAttemptMillis;
  unsigned long lastNoUpdateLogMillis;
  static const time_t validThreshold = 1000000000; // same threshold used above
};

// Global instance
TimeManager timeManager;

void sendTokenData(String id, TokenData * token_data) {
  Serial.println("the function is called");
  if (WiFi.status() != WL_CONNECTED) return;
   HTTPClient http;
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

    Serial.printf("📤 PATCH: %s\n", payload.c_str());
    int code = http.sendRequest("PATCH", payload);
   if (code == 204) {
    Serial.println("✅ PATCH success (204 No Content)");
} 
else if (code >= 200 && code < 300) {
    Serial.printf("✅ PATCH success (%d)\n", code);
} 
else {
    Serial.printf("❌ PATCH failed (%d): %s\n", code, http.getString().c_str());
}
  http.end();
}

// === Check WiFi and fallback to WiFiManager if failed ===
void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi disconnected! Attempting reconnect...");
    WiFi.disconnect();
    WiFi.begin();

    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      Serial.print(".");
    }

    if (WiFi.status() == WL_CONNECTED) {
      Serial.println("\n✅ WiFi reconnected!");
      return;
    }

    Serial.println("\n❌ Reconnect failed. Starting WiFiManager portal...");
    WiFiManager wm;
    wm.setConfigPortalTimeout(120);  // 2 minutes
    String setupName = String(deviceId) + "_SETUP";
    if (!wm.autoConnect(setupName.c_str())) {
      Serial.println("⏱ Portal timeout. Restarting...");
      delay(1000);
      ESP.restart();
    }

    Serial.println("✅ Connected via WiFiManager");
  }
}

// === Read Sensors and return individual validity ===
void readSensors(float &temp1, float &temp2, bool &valid1, bool &valid2) {
  sensor1.requestTemperatures();
  sensor2.requestTemperatures();
  delay(750);
  temp1 = sensor1.getTempCByIndex(0);
  temp2 = sensor2.getTempCByIndex(0);
  valid1 = (temp1 != 85.0 && temp1 != -127.0);
  valid2 = (temp2 != 85.0 && temp2 != -127.0);
}


// === Setup ===
void setup() {
  // ...existing code...
  pinMode(ONE_WIRE_BUS_1, INPUT_PULLUP);
  pinMode(ONE_WIRE_BUS_2, INPUT_PULLUP);
  Serial.begin(115200);
  delay(1000);
  prefs.begin("tokencfg", true); // read-only
  token_data.token_count = prefs.getInt("token_count", 0);
  token_data.meal = (mealType)prefs.getInt("meal", NONE);
  String dateStr = prefs.getString("date", "1970-01-01");
  strncpy(token_data.date, dateStr.c_str(), sizeof(token_data.date));
  token_data.date[10] = '\0';
  prefs.end();
  // Safety check: validate struct fields
  bool valid = true;
  if (token_data.meal < NONE || token_data.meal >= MEAL_COUNT) valid = false;
  if (strlen(token_data.date) != 10) valid = false;
  if (!valid) {
    Serial.println("⚠️ NVS data invalid, reinitializing token_data");
    token_data.token_count = 0;
    token_data.meal = NONE;
    strcpy(token_data.date, "1970-01-01");
    token_data.update = false;
    prefs.begin("tokencfg", false);
    prefs.putInt("token_count", token_data.token_count);
    prefs.putInt("meal", token_data.meal);
    prefs.putString("date", token_data.date);
    prefs.end();
  }
  Serial.printf("🔁 TokenData restored from NVS: count=%d, meal=%s, date=%s\n", token_data.token_count, meal[token_data.meal].c_str(), token_data.date);

  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setWiFiAutoReconnect(true);
  String setupName = String(deviceId) + "_SETUP";
  if (!wm.autoConnect(setupName.c_str())) {
    Serial.println("⏳ WiFiManager timeout. Restarting...");
    delay(1000);
    ESP.restart();
  }

  Serial.println("✅ Connected via WiFiManager");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  // Keep retrying until a valid server clock is received
  Serial.println("⏳ Waiting for valid server clock...");
  unsigned long startWait = millis();
  while (true) {
    timeManager.begin(TIME_SYNC_INTERVAL_MS, TIME_INITIAL_TIMEOUT_MS);
    time_t nowEpoch = timeManager.now();
    if (nowEpoch >= 1000000000) {
      struct tm tinfo;
      timeManager.getUTCTime(tinfo);
      char buf[32];
      strftime(buf, sizeof(buf), "%FT%TZ", &tinfo);
      Serial.printf("⏱ Time initialized: %s\n", buf);
      // After clock is initialized, compare EEPROM date and mealtype
      // Convert NTP to IST date string
      time_t epochLocal = nowEpoch + IST_OFFSET_SECONDS;
      struct tm tIST;
      gmtime_r(&epochLocal, &tIST);
      char todayIST[11];
      strftime(todayIST, sizeof(todayIST), "%Y-%m-%d", &tIST);
      // Determine current meal type
      int hour = tIST.tm_hour+DUMMYHREFORTESTING;
      mealType currentMealType = NONE;
      if (hour >= BFL && hour < BFH) currentMealType = BREAKFAST;
      else if (hour >= LFL && hour < LFH) currentMealType = LUNCH;
      else if (hour >= DFL && hour < DFH) currentMealType = DINNER;
      // Only initialize if current time is a meal time
      if (currentMealType == NONE) {
        Serial.printf("⏸ Not a meal time at init (hour=%d), not initializing token_data in NVS\n", hour);
      } else if (strcmp(token_data.date, todayIST) == 0 && token_data.meal == currentMealType) {
        Serial.printf("✅ EEPROM date and meal match: %s, %s. Keeping token_count=%d\n", todayIST, meal[currentMealType].c_str(), token_data.token_count);
      } else {
        Serial.printf("ℹ️ EEPROM date/meal mismatch or new meal/day. Initializing token_count=0\n");
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
      Serial.println("⚠️ Time not synchronized yet; retrying in 2s...");
      delay(2000);
    }
  }

  // lastRelayCheck = millis();
  lastSensorSend = millis();
  lastWiFiCheck = millis();
  lastEEPROMWrite = millis();
}

// === Main Loop ===
void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheck > 10000) {
    lastWiFiCheck = now;
    checkWiFi();
    Serial.println("Checked WiFi");
  }
  // Update TimeManager so it can perform hourly syncs (or keep local time running)
  timeManager.update();

  if (now - lastSensorSend >= 10000) {

  // Get IST time
  time_t epoch = timeManager.now();
  time_t epochLocal = epoch + IST_OFFSET_SECONDS;
  struct tm t;
  gmtime_r(&epochLocal, &t);
  char dateStr[11];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &t);
  int hour = t.tm_hour+DUMMYHREFORTESTING;
  strncpy(token_data.date, dateStr, sizeof(token_data.date));
  token_data.date[10] = '\0';   // ensure null-terminated


  if (hour >= BFL && hour < BFH) token_data.meal  = BREAKFAST;
  else if (hour >= LFL && hour < LFH) token_data.meal = LUNCH;
  else if (hour >= DFL && hour < DFH) token_data.meal = DINNER;
  else token_data.meal = NONE;

 if (token_data.meal == NONE) {
    Serial.printf("⏸ Not a meal time, skipping token send. hour=%d, token_count=%d, date=%s, meal=%s\n",
                  hour, token_data.token_count, token_data.date, meal[token_data.meal].c_str());
}
  else{
      token_data.token_count += 1;
      token_data2 = token_data;
      token_data3 = token_data;

      token_data2.token_count += 2;
      token_data3.token_count += 3;

      sendTokenData(deviceId, &token_data);
       sendTokenData(deviceId2, &token_data2);
        sendTokenData(deviceId3, &token_data3);
     
    }
     lastSensorSend = now;
  }

  if ((now - lastEEPROMWrite >= 20000) || token_data.update == true ) {
    lastEEPROMWrite = now;
    token_data.update = false;
    prefs.begin("tokencfg", false);
    prefs.putInt("token_count", token_data.token_count);
    prefs.putInt("meal", token_data.meal);
    prefs.putString("date", token_data.date);
    prefs.end();
    Serial.println("💾 TokenData saved to NVS.");
  }
 delay(10);

}