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
#include <EEPROM.h>

// === EEPROM Setup ===
#define EEPROM_SIZE 1
#define EEPROM_RELAY_ADDR 0

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
#define TIME_INITIAL_TIMEOUT_MS 15000UL // initial NTP wait (ms)
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

// Maximum schedules to load
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


// === Send Sensor Data ===
void sendSensorData(String id, int t1, int t2, bool valid1, bool valid2) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(postURL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));

  String payload = "{";
  payload += "\"sensor_id\":\"" + id + "\",";
  if (valid1) payload += "\"t1\":" + String(t1, 2) + ",";
  // payload += "\"relay1\":" + String(relay1 ? "true" : "false");
  payload += "}";

  Serial.println("📤 POST: " + payload);
  int code = http.POST(payload);
  if (code > 0) Serial.println("✅ Supabase: " + http.getString());
  else Serial.println("❌ POST failed");

  http.end();
}


void sendTokenData(String id, int tokenCount) {
  Serial.println("the function is called");
  if (WiFi.status() != WL_CONNECTED) return;

  // Get IST time
  time_t epoch = timeManager.now();
  time_t epochLocal = epoch + IST_OFFSET_SECONDS;
  struct tm t;
  gmtime_r(&epochLocal, &t);
  char dateStr[11];
  strftime(dateStr, sizeof(dateStr), "%Y-%m-%d", &t);
  int hour = t.tm_hour;

  String meal;
  if (hour >= 7 && hour < 9) meal = "breakfast";
  else if (hour >= 11 && hour < 14) meal = "lunch";
  else if (hour >= 18 && hour < 21) meal = "dinner";
  else meal = "none";

  if (meal == "none") {
    Serial.println("⏸ Not a meal time, skipping token send.");
    return;
  }

   HTTPClient http;
    String url = String(postURL) + "?sensor_id=eq." + id + "&date=eq." + dateStr;
    http.begin(url);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("apikey", apikey);
    http.addHeader("Authorization", "Bearer " + String(apikey));
    http.addHeader("Prefer", "return=representation");
    StaticJsonDocument<256> doc;
    doc[meal] = tokenCount;
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
  EEPROM.begin(EEPROM_SIZE);
  delay(5);
  pinMode(ONE_WIRE_BUS_1, INPUT_PULLUP);
  pinMode(ONE_WIRE_BUS_2, INPUT_PULLUP);
  pinMode(RELAY1_PIN, OUTPUT);

  relayState1 = EEPROM.read(EEPROM_RELAY_ADDR) == 1;
  digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
  Serial.begin(115200);
  delay(1000);
  Serial.printf("🔁 Relay restored from EEPROM: %s\n", relayState1 ? "ON" : "OFF");

  sensor1.begin();
  sensor2.begin();

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
  // Initialize TimeManager: it will attempt an immediate sync and then
  // keep local time running and sync every `TIME_SYNC_INTERVAL_MS`.
  timeManager.begin(TIME_SYNC_INTERVAL_MS, TIME_INITIAL_TIMEOUT_MS);
  // Ensure retry interval is set (used when sync fails)
  timeManager.setRetryInterval(TIME_RETRY_INTERVAL_MS);
  time_t nowEpoch = timeManager.now();
  if (nowEpoch >= 1000000000) {
    struct tm tinfo;
    timeManager.getUTCTime(tinfo);
    char buf[32];
    strftime(buf, sizeof(buf), "%FT%TZ", &tinfo);
    Serial.printf("⏱ Time initialized: %s\n", buf);
    // scheduleAllowed = true;
  } else {
    Serial.println("⚠️ Time not synchronized yet; attempting one additional NTP sync at boot...");
    // Try one more sync attempt at boot to obtain a valid time
    if (timeManager.trySync(TIME_INITIAL_TIMEOUT_MS)) {
      time_t newEpoch = timeManager.now();
      if (newEpoch >= 1000000000) {
        struct tm tinfo;
        timeManager.getUTCTime(tinfo);
        char buf[32];
        strftime(buf, sizeof(buf), "%FT%TZ", &tinfo);
        Serial.printf("✅ NTP sync success on second attempt: %s\n", buf);
        // scheduleAllowed = true;
      } else {
        Serial.println("❌ NTP sync returned invalid time on second attempt; schedule disabled until NTP available");
        // scheduleAllowed = false;
      }
    } else {
      Serial.println("❌ Additional NTP sync attempt failed; schedule disabled until NTP available");
      // scheduleAllowed = false;
    }
  }

  // relayState1 = fetchRelayCommand(deviceId, "relay1", relayState1);
  // digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
  // Serial.printf("🔄 Relay updated from Supabase: %s\n", relayState1 ? "ON" : "OFF");

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
    static int t1=0, t2=10, t3=20;  
    bool valid1, valid2;
    valid1= true;
    valid2= true;

    // readSensors(t1, t2, valid1, valid2);
    if (valid1 || valid2) {
      Serial.println("📊 Periodic sensor update.");
    } else {
      Serial.println("⚠️ Both sensors invalid — sending relay only.");
    }
    t1++;
    t2++;
    t3++;
    sendTokenData(deviceId, t1);

    lastSensorSend = now;
  }
  if (now - lastEEPROMWrite >= 10000) {
    lastEEPROMWrite = now;
    uint8_t stored = EEPROM.read(EEPROM_RELAY_ADDR);
    if (stored != (relayState1 ? 1 : 0)) {
      EEPROM.write(EEPROM_RELAY_ADDR, relayState1 ? 1 : 0);
      EEPROM.commit();
      Serial.println("💾 Relay state saved to EEPROM.");
    }
  }
 delay(10);
}