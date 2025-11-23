#include <WiFiManager.h> 
#include <WiFi.h>
#include <SPI.h>
#include <time.h>
#include <HTTPClient.h>
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
#define TIME_INITIAL_TIMEOUT_MS 5000UL // initial NTP wait (ms)
#endif
#ifndef TIME_SYNC_ATTEMPT_TIMEOUT_MS
#define TIME_SYNC_ATTEMPT_TIMEOUT_MS 3000UL // single sync attempt timeout (ms) to avoid long blocking
#endif

// === Supabase API Info ===
// === Supabase API Info ===
const char *getURL = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/commands";
const char *postURL = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/sensor_data";
const char *apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5ra3dkY3NvaWp3Y2JncXJ1YmxnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjM0OTg2MDgsImV4cCI6MjA3OTA3NDYwOH0.z3P1a_zOvjm1EGAggj6JS5u0Eo091mUcZ0wXyfEge-w";

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

// === Fetch Relay Command ===
bool fetchRelayCommand(const char *sensor_id, const char *target, bool currentState) {
  HTTPClient http;
  String url = String(getURL) + "?sensor_id=eq." + sensor_id + "&target=eq." + target + "&order=issued_at.desc&limit=1";
  http.begin(url);
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));

  int httpCode = http.GET();
  if (httpCode == 200) {
    String response = http.getString();
    int tsStart = response.indexOf("\"issued_at\":\"") + 13;
    int tsEnd = response.indexOf("\"", tsStart);
    if (tsStart > 12 && tsEnd > tsStart) {
      String timestampStr = response.substring(tsStart, tsEnd);
      struct tm tm;
      if (sscanf(timestampStr.c_str(), "%4d-%2d-%2dT%2d:%2d:%2d",
                 &tm.tm_year, &tm.tm_mon, &tm.tm_mday,
                 &tm.tm_hour, &tm.tm_min, &tm.tm_sec) == 6) {
        tm.tm_year -= 1900;
        tm.tm_mon -= 1;
        time_t issuedEpoch = mktime(&tm);
        time_t nowEpoch = time(nullptr);
        if (difftime(nowEpoch, issuedEpoch) > 120) return currentState;
        if (response.indexOf("\"state\":true") != -1) return true;
        if (response.indexOf("\"state\":false") != -1) return false;
      }
    }
  }
  http.end();
  return currentState;
}

// === Send Sensor Data ===
void sendSensorData(String id, float t1, float t2, bool valid1, bool valid2, bool relay1) {
  if (WiFi.status() != WL_CONNECTED) return;

  HTTPClient http;
  http.begin(postURL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));

  String payload = "{";
  payload += "\"sensor_id\":\"" + id + "\",";
  if (valid1) payload += "\"t1\":" + String(t1, 2) + ",";
  if (valid2) payload += "\"t2\":" + String(t2, 2) + ",";
  payload += "\"relay1\":" + String(relay1 ? "true" : "false");
  payload += "}";

  Serial.println("📤 POST: " + payload);
  int code = http.POST(payload);
  if (code > 0) Serial.println("✅ Supabase: " + http.getString());
  else Serial.println("❌ POST failed");

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
    if (!wm.autoConnect("AC_2_SETUP")) {
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

// === TimeManager ===
// Keeps a local epoch running using millis() and periodically attempts to sync
// with NTP. If offline or NTP fails, local time continues advancing.
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
    unsigned long attemptStart = millis();
    time_t epoch = fetchNetworkTime(timeoutMs);
    unsigned long took = millis() - attemptStart;
    if (epoch == (time_t)-1) {
      Serial.printf("⚠️ TimeManager: NTP attempt timed out after %lums\n", took);
      lastSyncSuccess = false;
      lastAttemptMillis = millis();
      return false;
    }
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
  if (!wm.autoConnect("AC_2_SETUP")) {
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
  } else {
    Serial.println("⚠️ Time not synchronized yet; using local clock.");
  }

  relayState1 = fetchRelayCommand("ac_2", "relay1", relayState1);
  digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
  Serial.printf("🔄 Relay updated from Supabase: %s\n", relayState1 ? "ON" : "OFF");

  lastRelayCheck = millis();
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
  }

  // Update TimeManager so it can perform hourly syncs (or keep local time running)
  timeManager.update();

  if (now - lastRelayCheck >= 5000) {
    lastRelayCheck = now;
    bool newState = fetchRelayCommand("ac_2", "relay1", relayState1);
    if (newState != relayState1) {
      relayState1 = newState;
      digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
      Serial.printf("🔄 Relay changed: %s\n", relayState1 ? "ON" : "OFF");

      float t1, t2;
      bool valid1, valid2;
      readSensors(t1, t2, valid1, valid2);
      sendSensorData("ac_2", t1, t2, valid1, valid2, relayState1);
      lastSensorSend = now;
    }
  }

  if (now - lastSensorSend >= 40000) {
    float t1, t2;
    bool valid1, valid2;
    readSensors(t1, t2, valid1, valid2);
    if (valid1 || valid2) {
      Serial.println("📊 Periodic sensor update.");
    } else {
      Serial.println("⚠️ Both sensors invalid — sending relay only.");
    }
    sendSensorData("ac_2", t1, t2, valid1, valid2, relayState1);
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
