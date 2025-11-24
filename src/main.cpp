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

// How often to dump the schedule table to Serial (default 5 minutes)
#ifndef SCHEDULE_DUMP_INTERVAL_MS
#define SCHEDULE_DUMP_INTERVAL_MS 60000UL
#endif

// Device identifier used to select schedules in Supabase
const char *deviceId = "ac_3";

// Maximum schedules to load
#define MAX_SCHEDULES 8

// India Standard Time offset from UTC in seconds (+5:30)
static const long IST_OFFSET_SECONDS = 5 * 3600 + 30 * 60;
// === Supabase API Info ===
// === Supabase API Info ===
const char *getURL = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/commands";
const char *getURLschedule = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/schedule";
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
// lastScheduleCheck is used to run schedule checks every minute
unsigned long lastScheduleCheck = 0;
unsigned long lastScheduleDump = 0;
// Whether schedule logic is allowed (requires valid NTP time)
bool scheduleAllowed = false;
// When a schedule OFF event occurs we latch timers disabled until schedule ON or manual ON
bool timersDisabledBySchedule = false;

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
    if (!wm.autoConnect("AC_3_SETUP")) {
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

// === Schedule engine (placed after TimeManager so it can use its API) ===
// === Schedule state ===
struct ScheduleRow {
  bool enable = false;
  String setting = ""; // "schedule" or "timer"
  long row_id = 0; // Added to track Supabase row id
  // schedule times (time of day)
  int on_h = 0, on_m = 0, on_s = 0;
  int off_h = 0, off_m = 0, off_s = 0;
  // timer durations (seconds)
  unsigned long on_duration_s = 0;
  unsigned long off_duration_s = 0;
  // weekdays: 0=Sun,1=Mon,...6=Sat
  bool weekday[7] = {false, false, false, false, false, false, false};
  // timer runtime state
  bool timer_state = false; // current state during timer mode (true=ON) (unchanged)
  unsigned long last_toggle_ms = 0; // last change timestamp for timer mode
  bool initialized = false; // whether we've initialized timer_state
};

// Array to hold multiple schedules fetched from Supabase
static ScheduleRow scheduleRows[MAX_SCHEDULES];
static int scheduleCount = 0;

// Helper: parse interval strings like "08:00:00" into h,m,s
static void parseIntervalToHMS(const String &s, int &h, int &m, int &sec) {
  h = m = sec = 0;
  if (s.length() == 0) return;
  // Accept formats: HH:MM or HH:MM:SS
  int first = s.indexOf(':');
  int second = s.indexOf(':', first + 1);
  if (first < 0) return;
  h = s.substring(0, first).toInt();
  if (second < 0) {
    m = s.substring(first + 1).toInt();
    sec = 0;
  } else {
    m = s.substring(first + 1, second).toInt();
    sec = s.substring(second + 1).toInt();
  }
}

// Helper: parse interval string to total seconds (for timer durations)
static unsigned long parseIntervalToSeconds(const String &s) {
  int h, m, sec;
  parseIntervalToHMS(s, h, m, sec);
  return (unsigned long)h * 3600UL + (unsigned long)m * 60UL + (unsigned long)sec;
}

// Helpers to compute next ON/OFF epochs (UTC) for schedule rows using local IST
static time_t nextOnForScheduleRow(const ScheduleRow &r, time_t nowUtc) {
  time_t nowLocal = nowUtc + IST_OFFSET_SECONDS;
  struct tm localTm;
  gmtime_r(&nowLocal, &localTm);
  int cur_h = localTm.tm_hour;
  int cur_m = localTm.tm_min;
  int cur_s = localTm.tm_sec;
  // local midnight epoch
  time_t localMidnight = nowLocal - (cur_h * 3600 + cur_m * 60 + cur_s);
  for (int d = 0; d < 7; ++d) {
    int dayIndex = (localTm.tm_wday + d) % 7; // weekday index for candidate day
    if (!r.weekday[dayIndex]) continue;
    time_t candidateLocal = localMidnight + (time_t)d * 86400 + (time_t)r.on_h * 3600 + (time_t)r.on_m * 60 + (time_t)r.on_s;
    if (candidateLocal > nowLocal) {
      // convert local epoch back to UTC
      return candidateLocal - IST_OFFSET_SECONDS;
    }
  }
  // fallback: return 0 if none
  return (time_t)0;
}

static time_t nextOffForScheduleRow(const ScheduleRow &r, time_t nowUtc) {
  time_t nowLocal = nowUtc + IST_OFFSET_SECONDS;
  struct tm localTm;
  gmtime_r(&nowLocal, &localTm);
  int cur_h = localTm.tm_hour;
  int cur_m = localTm.tm_min;
  int cur_s = localTm.tm_sec;
  time_t localMidnight = nowLocal - (cur_h * 3600 + cur_m * 60 + cur_s);
  for (int d = 0; d < 7; ++d) {
    int dayIndex = (localTm.tm_wday + d) % 7;
    if (!r.weekday[dayIndex]) continue;
    time_t candidateLocal = localMidnight + (time_t)d * 86400 + (time_t)r.off_h * 3600 + (time_t)r.off_m * 60 + (time_t)r.off_s;
    if (candidateLocal > nowLocal) {
      return candidateLocal - IST_OFFSET_SECONDS;
    }
  }
  return (time_t)0;
}

// For timer rows compute next change epoch (UTC). If timer_state==true, next change is OFF, else next is ON.
static time_t nextChangeForTimerRow(const ScheduleRow &r, unsigned long nowMs) {
  if (r.on_duration_s == 0 && r.off_duration_s == 0) return (time_t)0;
  unsigned long lastToggle = r.last_toggle_ms;
  if (lastToggle == 0) {
    // not initialized: assume it will toggle after on_duration from now if initialized as ON
    if (r.timer_state) {
      return (time_t)((time(nullptr)) + (time_t)r.on_duration_s);
    } else {
      return (time_t)((time(nullptr)) + (time_t)r.off_duration_s);
    }
  }
  unsigned long elapsed = (nowMs - lastToggle) / 1000UL;
  if (r.timer_state) {
    if (r.on_duration_s > elapsed) return (time_t)(time(nullptr) + (time_t)(r.on_duration_s - elapsed));
    else return (time_t)(time(nullptr));
  } else {
    if (r.off_duration_s > elapsed) return (time_t)(time(nullptr) + (time_t)(r.off_duration_s - elapsed));
    else return (time_t)(time(nullptr));
  }
}

// Compute and print next ON and OFF times (local IST) across all enabled schedules/timers
static void printNextOnOffTimes() {
  if (scheduleCount <= 0) {
    Serial.println("ℹ️ No schedules loaded");
    return;
  }
  time_t nowUtc = timeManager.now();
  if (nowUtc <= 0) {
    Serial.println("⚠️ Time not available — can't compute next events");
    return;
  }
  unsigned long nowMs = millis();

  time_t bestNextOn = (time_t)0;
  time_t bestNextOff = (time_t)0;

  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    if (!r.enable) continue;
    if (r.setting.equalsIgnoreCase("schedule")) {
      time_t candOn = nextOnForScheduleRow(r, nowUtc);
      time_t candOff = nextOffForScheduleRow(r, nowUtc);
      if (candOn != 0 && (bestNextOn == 0 || candOn < bestNextOn)) bestNextOn = candOn;
      if (candOff != 0 && (bestNextOff == 0 || candOff < bestNextOff)) bestNextOff = candOff;
    } else if (r.setting.equalsIgnoreCase("timer")) {
      // timer rows: derive next on/off from timer_state and durations
      if (r.timer_state) {
        // currently ON; next change is OFF
        time_t candOff = nextChangeForTimerRow(r, nowMs);
        if (candOff != 0 && (bestNextOff == 0 || candOff < bestNextOff)) bestNextOff = candOff;
      } else {
        // currently OFF; next change is ON
        time_t candOn = nextChangeForTimerRow(r, nowMs);
        if (candOn != 0 && (bestNextOn == 0 || candOn < bestNextOn)) bestNextOn = candOn;
      }
    }
  }

  auto printLocal = [&](time_t t, const char *label) {
    if (t == 0) {
      Serial.printf("%s: none\n", label);
      return;
    }
    time_t local = t + IST_OFFSET_SECONDS;
    struct tm lt;
    gmtime_r(&local, &lt);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S IST", &lt);
    Serial.printf("%s: %s\n", label, buf);
  };

  Serial.println("=== Next schedule events (device: " + String(deviceId) + ") ===");
  printLocal(bestNextOn, "Next ON");
  printLocal(bestNextOff, "Next OFF");
  Serial.println("=== end next events ===");
}

// Print all ON/OFF events in the upcoming 24 hours (IST) across enabled schedules/timers
static void printNext24hSchedule() {
  if (scheduleCount <= 0) {
    Serial.println("ℹ️ No schedules loaded");
    return;
  }
  time_t nowUtc = timeManager.now();
  if (nowUtc <= 0) {
    Serial.println("⚠️ Time not available — can't compute next events");
    return;
  }
  unsigned long nowMs = millis();
  time_t nowLocal = nowUtc + IST_OFFSET_SECONDS;
  time_t endLocal = nowLocal + 86400; // 24 hours ahead in local time

  const int MAX_EVENTS = 256;
  time_t evTimes[MAX_EVENTS];
  int evTypes[MAX_EVENTS]; // 1 = ON, 0 = OFF
  long evRowId[MAX_EVENTS];
  int evCount = 0;

  // Collect schedule-based events
  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    if (!r.enable) continue;
    if (r.setting.equalsIgnoreCase("schedule")) {
      // check today and next day in local time
      for (int d = 0; d < 2; ++d) {
        // compute local midnight for today + d
        struct tm lt;
        gmtime_r(&nowLocal, &lt);
        int cur_h = lt.tm_hour, cur_m = lt.tm_min, cur_s = lt.tm_sec;
        time_t localMidnight = nowLocal - (cur_h * 3600 + cur_m * 60 + cur_s) + (time_t)d * 86400;
        int dayIndex = (lt.tm_wday + d) % 7;
        if (r.weekday[dayIndex]) {
          time_t onLocal = localMidnight + (time_t)r.on_h * 3600 + (time_t)r.on_m * 60 + (time_t)r.on_s;
          time_t offLocal = localMidnight + (time_t)r.off_h * 3600 + (time_t)r.off_m * 60 + (time_t)r.off_s;
          if (onLocal > nowLocal && onLocal <= endLocal && evCount < MAX_EVENTS) {
            evTimes[evCount] = onLocal - IST_OFFSET_SECONDS; // store as UTC
            evTypes[evCount] = 1;
            evRowId[evCount] = r.row_id;
            evCount++;
          }
          if (offLocal > nowLocal && offLocal <= endLocal && evCount < MAX_EVENTS) {
            evTimes[evCount] = offLocal - IST_OFFSET_SECONDS;
            evTypes[evCount] = 0;
            evRowId[evCount] = r.row_id;
            evCount++;
          }
        }
      }
    } else if (r.setting.equalsIgnoreCase("timer")) {
      // Simulate toggles starting from current state up to 24 hours
      bool curState = r.timer_state;
      unsigned long lastToggle = r.last_toggle_ms;
      unsigned long simNowMs = nowMs;
      unsigned long simLastToggleMs = lastToggle ? lastToggle : simNowMs;
      time_t simEventUtc = nowUtc;
      // limit to avoid runaway
      int iter = 0;
      while (evCount < MAX_EVENTS && iter < 64) {
        iter++;
        unsigned long elapsed = (simNowMs - simLastToggleMs) / 1000UL;
        unsigned long remaining = 0;
        if (curState) remaining = (r.on_duration_s > elapsed) ? (r.on_duration_s - elapsed) : 0;
        else remaining = (r.off_duration_s > elapsed) ? (r.off_duration_s - elapsed) : 0;
        time_t nextUtc;
        if (remaining == 0) {
          nextUtc = time(nullptr); // immediate toggle
        } else {
          nextUtc = nowUtc + (time_t)remaining;
        }
        time_t nextLocal = nextUtc + IST_OFFSET_SECONDS;
        if (nextLocal > nowLocal && nextLocal <= endLocal) {
          if (evCount < MAX_EVENTS) {
            evTimes[evCount] = nextUtc;
            evTypes[evCount] = curState ? 0 : 1; // if currently ON, next is OFF
            evRowId[evCount] = r.row_id;
            evCount++;
          }
        } else if (nextLocal > endLocal) {
          break;
        }
        // advance simulation
        simLastToggleMs = simNowMs + remaining * 1000UL;
        simNowMs = simLastToggleMs;
        nowUtc = nextUtc;
        curState = !curState;
      }
    }
  }

  // Simple insertion sort by evTimes
  for (int i = 1; i < evCount; ++i) {
    time_t keyT = evTimes[i];
    int keyType = evTypes[i];
    long keyId = evRowId[i];
    int j = i - 1;
    while (j >= 0 && evTimes[j] > keyT) {
      evTimes[j + 1] = evTimes[j];
      evTypes[j + 1] = evTypes[j];
      evRowId[j + 1] = evRowId[j];
      j--;
    }
    evTimes[j + 1] = keyT;
    evTypes[j + 1] = keyType;
    evRowId[j + 1] = keyId;
  }

  Serial.println("=== Upcoming events (next 24h) ===");
  for (int i = 0; i < evCount; ++i) {
    time_t local = evTimes[i] + IST_OFFSET_SECONDS;
    struct tm lt;
    gmtime_r(&local, &lt);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S IST", &lt);
    Serial.printf("%s - %s (row id: %ld)\n", buf, evTypes[i] ? "ON" : "OFF", evRowId[i]);
  }
  if (evCount == 0) Serial.println("No events in the next 24 hours.");
  Serial.println("=== end upcoming events ===");
}

// Fetch all schedules for this device from Supabase and populate scheduleRows[]
static void fetchSchedulesFromSupabase() {
  // Preserve previous runtime state to avoid restarting timers on every fetch
  ScheduleRow prevRows[MAX_SCHEDULES];
  int prevCount = scheduleCount;
  for (int i = 0; i < prevCount && i < MAX_SCHEDULES; ++i) prevRows[i] = scheduleRows[i];
  scheduleCount = 0;
  if (WiFi.status() != WL_CONNECTED) return;
  HTTPClient http;
  String url = String(getURLschedule) + "?sensor_id=eq." + String(deviceId) + "&target=eq.relay1&order=id.asc";
  http.begin(url);
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));
  int code = http.GET();
  if (code != 200) {
    http.end();
    return;
  }
  String resp = http.getString();
  http.end();

  // Use ArduinoJson to parse the array of schedule rows
  const size_t capacity = 8192;
  DynamicJsonDocument doc(capacity);
  DeserializationError err = deserializeJson(doc, resp);
  if (err) {
    Serial.printf("❌ fetchSchedulesFromSupabase: JSON parse failed: %s\n", err.c_str());
    return;
  }

  if (!doc.is<JsonArray>()) return;
  JsonArray arr = doc.as<JsonArray>();
  for (JsonObject obj : arr) {
    if (scheduleCount >= MAX_SCHEDULES) break;
    ScheduleRow &r = scheduleRows[scheduleCount];
    r.enable = obj["enable"] | false;
    r.row_id = obj["id"] | 0;
    const char *s = obj["setting"] | "";
    r.setting = String(s);

    // parse timer_on_duration / timer_off_duration as strings when present
    if (!obj["timer_on_duration"].isNull()) {
      String val = String((const char *)obj["timer_on_duration"]);
      parseIntervalToHMS(val, r.on_h, r.on_m, r.on_s);
      r.on_duration_s = parseIntervalToSeconds(val);
    } else {
      r.on_h = r.on_m = r.on_s = 0;
      r.on_duration_s = 0;
    }
    if (!obj["timer_off_duration"].isNull()) {
      String val = String((const char *)obj["timer_off_duration"]);
      parseIntervalToHMS(val, r.off_h, r.off_m, r.off_s);
      r.off_duration_s = parseIntervalToSeconds(val);
    } else {
      r.off_h = r.off_m = r.off_s = 0;
      r.off_duration_s = 0;
    }

    // weekdays
    r.weekday[1] = obj["mon"] | false;
    r.weekday[2] = obj["tue"] | false;
    r.weekday[3] = obj["wed"] | false;
    r.weekday[4] = obj["thu"] | false;
    r.weekday[5] = obj["fri"] | false;
    r.weekday[6] = obj["sat"] | false;
    r.weekday[0] = obj["sun"] | false;

    // Attempt to restore runtime state from previous fetch (match by row_id)
    bool restored = false;
    for (int k = 0; k < prevCount; ++k) {
      if (prevRows[k].row_id != 0 && prevRows[k].row_id == r.row_id) {
        r.timer_state = prevRows[k].timer_state;
        r.last_toggle_ms = prevRows[k].last_toggle_ms;
        r.initialized = prevRows[k].initialized;
        restored = true;
        break;
      }
    }
    if (!restored) {
      r.timer_state = false;
      r.last_toggle_ms = 0;
      r.initialized = false;
    }

    scheduleCount++;
  }
}

// Read the entire schedule table from Supabase and print to Serial
static void printScheduleTable() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ printScheduleTable: WiFi not connected");
    return;
  }
  HTTPClient http;
  http.begin(getURLschedule);
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));
  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    // Minor formatting: put each object on its own line for readability
    resp.replace("},{", "},\n{");
    Serial.println("=== schedule table ===");
    Serial.println(resp);
    Serial.println("=== end schedule table ===");
  } else {
    Serial.printf("❌ printScheduleTable: HTTP GET failed, code=%d\n", code);
    if (code > 0) Serial.println(http.getString());
  }
  http.end();
}

// Fetch and print only enabled schedules for this device (enable = true)
static void printEnabledSchedules() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ printEnabledSchedules: WiFi not connected");
    return;
  }
  HTTPClient http;
  String url = String(getURLschedule) + "?sensor_id=eq." + String(deviceId) + "&target=eq.relay1&enable=eq.true&order=id.asc";
  http.begin(url);
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));
  int code = http.GET();
  if (code == 200) {
    String resp = http.getString();
    // Minor formatting for readability
    resp.replace("},{", "},\n{");
    Serial.println("=== enabled schedules for device: " + String(deviceId) + " ===");
    Serial.println(resp);
    Serial.println("=== end enabled schedules ===");
  } else {
    Serial.printf("❌ printEnabledSchedules: HTTP GET failed, code=%d\n", code);
    if (code > 0) Serial.println(http.getString());
  }
  http.end();
}

// Evaluate and apply schedule; call every 1 minute
static void checkSchedule() {
  if (!scheduleAllowed) return;

  // Fetch all schedules for this device
  fetchSchedulesFromSupabase();
  if (scheduleCount <= 0) return;

  time_t epoch = timeManager.now();
  if (epoch <= 0) return;
  // Use local IST time for schedule comparisons
  time_t epochUtc = epoch;
  time_t epochLocal = epochUtc + IST_OFFSET_SECONDS;
  struct tm t;
  gmtime_r(&epochLocal, &t);
  int today = t.tm_wday; // 0=Sun
  int cur_h = t.tm_hour;
  int cur_m = t.tm_min;

  bool anyOnTrigger = false;
  bool anyOffTrigger = false;
  int timerOnCount = 0;

  unsigned long nowMs = millis();

  for (int i = 0; i < scheduleCount; ++i) {
    ScheduleRow &r = scheduleRows[i];
    if (!r.enable) continue;
    if (r.setting.equalsIgnoreCase("schedule")) {
      if (!r.weekday[today]) continue;
      if (cur_h == r.on_h && cur_m == r.on_m) anyOnTrigger = true;
      if (cur_h == r.off_h && cur_m == r.off_m) anyOffTrigger = true;
    } else if (r.setting.equalsIgnoreCase("timer")) {
      // If timers are disabled by a previous schedule OFF event, skip timer toggles
      if (timersDisabledBySchedule) continue;
      // initialize timer state if needed
      if (!r.initialized) {
        r.timer_state = true;
        r.last_toggle_ms = nowMs;
        r.initialized = true;
      }
      unsigned long elapsed = (nowMs - r.last_toggle_ms) / 1000UL;
      if (r.timer_state) {
        if (r.on_duration_s > 0 && elapsed >= r.on_duration_s) {
          r.timer_state = false;
          r.last_toggle_ms = nowMs;
          Serial.printf("⏱ Timer row %ld: relay1 OFF at %lu ms\n", r.row_id, nowMs);
        }
      } else {
        if (r.off_duration_s > 0 && elapsed >= r.off_duration_s) {
          r.timer_state = true;
          r.last_toggle_ms = nowMs;
          Serial.printf("⏱ Timer row %ld: relay1 ON at %lu ms\n", r.row_id, nowMs);
        }
      }
      if (r.timer_state) timerOnCount++;
    }
  }

  static bool desired = relayState1; // default: no change
  // Event-driven behavior:
  // - If any OFF trigger occurred now, latch timers disabled and force OFF
  // - If any ON trigger occurred now, clear the latch and force ON
  if (anyOffTrigger) {
    timersDisabledBySchedule = true;
    desired = false; // OFF takes precedence and latches timers disabled
    Serial.println("⏱ Schedule OFF event: relay1 forced OFF and timers disabled");
  } else if (anyOnTrigger) {
    timersDisabledBySchedule = false;
    desired = true;
    Serial.println("⏱ Schedule ON event: relay1 forced ON and timers enabled");
  } else {
    // No schedule event this minute: timers only affect relay when not disabled
    if (!timersDisabledBySchedule) {
      if (timerOnCount > 0) desired = true;
    }
    // if timersDisabledBySchedule is true, timers have no effect until cleared by schedule ON or manual ON
  }

  if (desired != relayState1) {
    relayState1 = desired;
    digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
    Serial.printf("🔁 Schedule engine set relay: %s\n", relayState1 ? "ON" : "OFF");    
  }
  // Print upcoming ON/OFF times after evaluating schedules
  // printNextOnOffTimes();
  // printNext24hSchedule();
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
  if (!wm.autoConnect("AC_3_SETUP")) {
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
    scheduleAllowed = true;
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
        scheduleAllowed = true;
      } else {
        Serial.println("❌ NTP sync returned invalid time on second attempt; schedule disabled until NTP available");
        scheduleAllowed = false;
      }
    } else {
      Serial.println("❌ Additional NTP sync attempt failed; schedule disabled until NTP available");
      scheduleAllowed = false;
    }
  }

  relayState1 = fetchRelayCommand("ac_3", "relay1", relayState1);
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

  // If schedule was disabled at boot, enable it automatically when NTP becomes available later
  if (!scheduleAllowed) {
    time_t maybe = timeManager.now();
    if (maybe >= 1000000000) {
      scheduleAllowed = true;
      Serial.println("✅ NTP now available — enabling schedule engine");
    }
  }

  // Schedule check: run every 1 minute
  if (now - lastScheduleCheck >= 60000UL) {
    lastScheduleCheck = now;
    checkSchedule();
  }

  // Periodically dump the schedule table to Serial (non-blocking)
  if (now - lastScheduleDump >= SCHEDULE_DUMP_INTERVAL_MS) {
    lastScheduleDump = now;
    // printScheduleTable();
    // printEnabledSchedules();
  }

  if (now - lastRelayCheck >= 5000) {
    lastRelayCheck = now;
    bool newState = fetchRelayCommand("ac_3", "relay1", relayState1);
    if (newState != relayState1) {
      // Manual command detected. Apply manual reset rules for timers.
      if (newState) {
        // Manual ON: clear schedule-driven timer disable latch and reset timers to ON from now
        timersDisabledBySchedule = false;
        for (int i = 0; i < scheduleCount; ++i) {
          ScheduleRow &r = scheduleRows[i];
          if (!r.enable) continue;
          if (r.setting.equalsIgnoreCase("timer")) {
            r.timer_state = true;
            r.last_toggle_ms = millis();
            r.initialized = true;
          }
        }
      } else {
        // Manual OFF: reset timers to OFF from now (but do NOT clear timersDisabledBySchedule)
        for (int i = 0; i < scheduleCount; ++i) {
          ScheduleRow &r = scheduleRows[i];
          if (!r.enable) continue;
          if (r.setting.equalsIgnoreCase("timer")) {
            r.timer_state = false;
            r.last_toggle_ms = millis();
            r.initialized = true;
          }
        }
      }

      relayState1 = newState;
      digitalWrite(RELAY1_PIN, relayState1 ? HIGH : LOW);
      Serial.printf("🔄 Relay changed (manual): %s\n", relayState1 ? "ON" : "OFF");

      float t1, t2;
      bool valid1, valid2;
      readSensors(t1, t2, valid1, valid2);
      sendSensorData("ac_3", t1, t2, valid1, valid2, relayState1);
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
    sendSensorData("ac_3", t1, t2, valid1, valid2, relayState1);
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
