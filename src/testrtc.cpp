#include <Arduino.h>
#include <WiFiManager.h>
#include <WiFi.h>
#include <time.h>
#include <Wire.h>
#include <RTClib.h>

// -----------------------------
// DS3231 on separate I2C bus
// -----------------------------
#define RTC_SDA_PIN 25
#define RTC_SCL_PIN 26

TwoWire rtcWire = TwoWire(1);
RTC_DS3231 rtc;

// =============================
// Debug switches
// =============================
// 0 = DO NOT set RTC from NTP (test coin-cell retention across power cycles)
// 1 = Set RTC from NTP (use once when needed)
#define SET_RTC_FROM_NTP 0

// IST offset (+5:30)
static const long IST_OFFSET_SECONDS = 5 * 3600 + 30 * 60;

// NTP validity threshold (roughly after year 2001)
static const time_t VALID_EPOCH_THRESHOLD = 1000000000;

// Wait for NTP time to become valid
static time_t waitForNtpEpoch(uint32_t timeoutMs) {
  const uint32_t start = millis();
  time_t nowEpoch = time(nullptr);
  while (nowEpoch < VALID_EPOCH_THRESHOLD && (millis() - start) < timeoutMs) {
    delay(100);
    nowEpoch = time(nullptr);
  }
  return (nowEpoch >= VALID_EPOCH_THRESHOLD) ? nowEpoch : (time_t)-1;
}

static void printEpochAsUTC(time_t epochUTC) {
  struct tm t;
  gmtime_r(&epochUTC, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S UTC", &t);
  Serial.println(buf);
}

static void printEpochAsIST(time_t epochUTC) {
  time_t epochIST = epochUTC + IST_OFFSET_SECONDS;
  struct tm t;
  gmtime_r(&epochIST, &t);
  char buf[32];
  strftime(buf, sizeof(buf), "%Y-%m-%d %I:%M:%S %p IST", &t);
  Serial.println(buf);
}

static void printRtcNow(const DateTime &dt) {
  char buf[40];
  snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d (RTC UTC)",
           dt.year(), dt.month(), dt.day(),
           dt.hour(), dt.minute(), dt.second());
  Serial.println(buf);

  time_t rtcEpochUTC = (time_t)dt.unixtime();
  Serial.print("RTC IST: ");
  printEpochAsIST(rtcEpochUTC);
}

void setup() {
  Serial.begin(460800);
  delay(300);
  Serial.println();
  Serial.println("=== ESP32 DS3231 RTC Debug (READ-ONLY RTC test) ===");

  // ---- Start RTC I2C bus on GPIO25/26 ----
  rtcWire.begin(RTC_SDA_PIN, RTC_SCL_PIN, 100000);
  if (!rtc.begin(&rtcWire)) {
    Serial.println("❌ DS3231 not detected on I2C(25/26).");
    Serial.println("Check wiring: VCC=3.3V, GND, SDA=25, SCL=26.");
    while (1) delay(1000);
  }
  Serial.println("✅ DS3231 detected.");

  // Print RTC immediately (this is what you care about after power cycle)
  Serial.println("📌 RTC time at boot:");
  printRtcNow(rtc.now());

  // Optional: show if RTC reports lostPower()
  if (rtc.lostPower()) {
    Serial.println("⚠️ rtc.lostPower() = true (battery missing/dead or time was lost previously).");
  } else {
    Serial.println("ℹ️ rtc.lostPower() = false");
  }

  // ---- Connect WiFi (WiFiManager) ----
  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setWiFiAutoReconnect(true);

  if (!wm.autoConnect("RTC_SETUP")) {
    Serial.println("❌ WiFiManager failed. Rebooting...");
    delay(1000);
    ESP.restart();
  }
  Serial.print("✅ WiFi connected. IP: ");
  Serial.println(WiFi.localIP());

  // ---- Start NTP (for comparison only) ----
  Serial.println("⏱ Starting NTP (comparison only)...");
  configTime(0, 0, "pool.ntp.org", "time.nist.gov"); // keep UTC in system

  time_t ntpEpochUTC = waitForNtpEpoch(15000); // 15 seconds max
  if (ntpEpochUTC == (time_t)-1) {
    Serial.println("❌ NTP time not available within timeout.");
    Serial.println("Continuing with RTC-only prints.");
    return;
  }

  Serial.print("✅ NTP UTC: ");
  printEpochAsUTC(ntpEpochUTC);
  Serial.print("✅ NTP IST: ");
  printEpochAsIST(ntpEpochUTC);

#if SET_RTC_FROM_NTP
  // ---- Set RTC from NTP UTC ----
  Serial.println("🔁 Setting DS3231 from NTP (UTC)...");
  rtc.adjust(DateTime((uint32_t)ntpEpochUTC));

  // ---- Read back immediately ----
  Serial.println("✅ RTC readback after setting:");
  printRtcNow(rtc.now());
#else
  Serial.println("🚫 RTC write is DISABLED (SET_RTC_FROM_NTP=0).");
  Serial.println("✅ Power-cycle test: RTC should keep running on coin cell.");
#endif
}

void loop() {
  // Print RTC time every 1 second
  static uint32_t lastMs = 0;
  if (millis() - lastMs >= 1000) {
    lastMs = millis();
    Serial.println("------------------------------");
    printRtcNow(rtc.now());
  }
}
