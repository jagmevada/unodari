#include <WiFiManager.h>
#include <WiFi.h>
#include <time.h>
#include <HTTPClient.h>
#include <EEPROM.h>
#include <Wire.h>
#include <Adafruit_SHT31.h>
#include <sps30.h>

// === EEPROM Setup ===
#define EEPROM_SIZE 2
#define EEPROM_RELAY1_ADDR 0
#define EEPROM_RELAY2_ADDR 1

// === Supabase API Info ===
const char *getURL = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/commands";
const char *postURL = "https://nkkwdcsoijwcbgqrublg.supabase.co/rest/v1/sensor_data";
const char *apikey = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJpc3MiOiJzdXBhYmFzZSIsInJlZiI6Im5ra3dkY3NvaWp3Y2JncXJ1YmxnIiwicm9sZSI6ImFub24iLCJpYXQiOjE3NjM0OTg2MDgsImV4cCI6MjA3OTA3NDYwOH0.z3P1a_zOvjm1EGAggj6JS5u0Eo091mUcZ0wXyfEge-w";

// === GPIO Definitions ===
#define RELAY1_PIN 32 // Air Purifier
#define RELAY2_PIN 33 // Dehumidifier

// === I2C Buses ===
TwoWire I2CBus1 = TwoWire(0); // GPIO 21/22
TwoWire I2CBus2 = TwoWire(1); // GPIO 25/26
Adafruit_SHT31 sht1;
Adafruit_SHT31 sht2;

// === SPS30 ===
SPS30 sps30;
struct sps_values pmData;

bool relayState1 = false;
bool relayState2 = true;
bool nosps = 0; // Flag for SPS30 detection
unsigned long lastRelayCheck = 0;
unsigned long lastSensorSend = 0;
unsigned long lastWiFiCheck = 0;
unsigned long lastEEPROMWrite = 0;

// === WiFi and Supabase ===
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

unsigned long lastSPSRead = 0;
bool readSPS30(struct sps_values &data) {
  if (millis() - lastSPSRead < 1100) {
    // Too soon to read again; use previously stored values
    data = pmData;  // Return last known good value
    return true;
  }

  for (uint8_t tries = 0; tries < 3; tries++) {
    if (sps30.GetValues(&data) == SPS30_ERR_OK) {
      pmData = data;                // Cache it
      lastSPSRead = millis();       // Timestamp
      return true;
    }
    Serial.println("❌ SPS30 read failed, retrying...");
    delay(200);  // Brief wait before retry
  }

  return false;  // All tries failed
}


void sendSensorData(String id, float t1, float t2, float rh1, float rh2, bool v1, bool v2, bool r1, bool r2) {
  if (WiFi.status() != WL_CONNECTED) return;
sps_values currentPM;
bool pmOK =0;
if(!nosps)
pmOK= readSPS30(currentPM);

  HTTPClient http;
  http.begin(postURL);
  http.addHeader("Content-Type", "application/json");
  http.addHeader("apikey", apikey);
  http.addHeader("Authorization", "Bearer " + String(apikey));

  String payload = "{";
  payload += "\"sensor_id\":\"" + id + "\",";

  // Temperature & RH
  payload += "\"t1\":" + (v1 ? String(t1, 2) : "null") + ",";
  payload += "\"t2\":" + (v2 ? String(t2, 2) : "null") + ",";
  // Send individual relative humidity values for each sensor
  payload += "\"rh1\":" + (v1 ? String(rh1, 2) : "null") + ",";
  payload += "\"rh2\":" + (v2 ? String(rh2, 2) : "null") + ",";

  // SPS30 Mass + Number concentrations
  if (pmOK) {
    payload += "\"pm1\":" + String(pmData.MassPM1, 1) + ",";
    payload += "\"pm25\":" + String(pmData.MassPM2, 1) + ",";
    payload += "\"pm10\":" + String(pmData.MassPM10, 1) + ",";
    payload += "\"avg_particle_size\":" + String(pmData.PartSize, 2) + ",";
    payload += "\"nc0_5\":" + String((int)pmData.NumPM0) + ",";
    payload += "\"nc1_0\":" + String((int)pmData.NumPM1) + ",";
    payload += "\"nc2_5\":" + String((int)pmData.NumPM2) + ",";
    payload += "\"nc10\":" + String((int)pmData.NumPM10) + ",";
  } else {
    payload += "\"pm1\":null,\"pm25\":null,\"pm10\":null,\"avg_particle_size\":null,";
    payload += "\"nc0_5\":null,\"nc1_0\":null,\"nc2_5\":null,\"nc10\":null,";
  }

  // Relays
  payload += "\"relay1\":" + String(r1 ? "true" : "false") + ",";
  payload += "\"relay2\":" + String(r2 ? "true" : "false");

  payload += "}";


  int code = http.POST(payload);
  Serial.println("📤 POST: " + payload);
if (code == 200 || code == 201) {
  Serial.println("✅ Supabase: POST success");
} else {
  Serial.printf("❌ Supabase POST failed. Code: %d, Body: %s\n", code, http.getString().c_str());
}
  http.end();
}

bool readSensors(float &t1, float &t2, float &rh1, float &rh2, bool &v1, bool &v2) {
  Wire = I2CBus1;
  t1 = sht1.readTemperature();
  rh1 = sht1.readHumidity();
  Serial.println("SHT1: T=" + String(t1, 2) + "°C, RH=" + String(rh1, 2) + "%");
  Wire = I2CBus2;
  t2 = sht2.readTemperature();
  rh2 = sht2.readHumidity();
  Serial.println("SHT2: T=" + String(t2, 2) + "°C, RH=" + String(rh2, 2) + "%");
  v1 = !isnan(t1) && !isnan(rh1);
  v2 = !isnan(t2) && !isnan(rh2);
  return v1 || v2;
}

void checkWiFi() {
  if (WiFi.status() != WL_CONNECTED) {
    Serial.println("⚠️ WiFi disconnected! Reconnecting...");
    WiFi.disconnect();
    WiFi.begin();
    unsigned long start = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - start < 10000) {
      delay(500);
      Serial.print(".");
    }
    if (WiFi.status() != WL_CONNECTED) {
      Serial.println("\n❌ WiFi reconnect failed. Launching portal...");
      WiFiManager wm;
      wm.setConfigPortalTimeout(120);
      if (!wm.autoConnect("ECS_2_SETUP")) {
        Serial.println("⏳ Portal timeout. Restarting...");
        ESP.restart();
      }
    } else {
      Serial.println("✅ Reconnected to WiFi");
    }
  }
}

void setup() {
  EEPROM.begin(EEPROM_SIZE);
  delay(5);
  Serial.begin(115200);

  pinMode(RELAY1_PIN, OUTPUT);
  pinMode(RELAY2_PIN, OUTPUT);
  relayState1 = EEPROM.read(EEPROM_RELAY1_ADDR) == 1;
  relayState2 = EEPROM.read(EEPROM_RELAY2_ADDR) == 1;
  digitalWrite(RELAY1_PIN, relayState1 ? LOW : HIGH);
  digitalWrite(RELAY2_PIN, relayState2 ? LOW : HIGH);

  I2CBus1.begin(21, 22);
  I2CBus2.begin(25, 26);

  Wire = I2CBus1;
  sht1.begin(0x44);
  Wire = I2CBus2;
  sht2.begin(0x44);

  // === SPS30 Init ===
  if (!sps30.begin(&I2CBus1)) Serial.println("❌ SPS30 I2C init failed");
  else if (!sps30.probe()) {
  Serial.println("❌ SPS30 not detected");
  nosps=1;}
  else {
    Serial.println("✅ SPS30 detected");
    nosps = 0;
    sps30.reset();
    delay(100);
    if (sps30.start()) Serial.println("✅ SPS30 started");
    else Serial.println("❌ SPS30 start failed");
  }

  WiFiManager wm;
  wm.setConfigPortalTimeout(120);
  wm.setWiFiAutoReconnect(true);
  if (!wm.autoConnect("ECS_2_SETUP")) {
    Serial.println("❌ WiFiManager failed. Restarting...");
    ESP.restart();
  }

  configTime(0, 0, "pool.ntp.org", "time.nist.gov");
  delay(1000);

  relayState1 = fetchRelayCommand("ecs_1", "relay1", relayState1);
  relayState2 = fetchRelayCommand("ecs_1", "relay2", relayState2);
  digitalWrite(RELAY1_PIN, relayState1 ? LOW : HIGH);
  digitalWrite(RELAY2_PIN, relayState2 ? LOW : HIGH);

  lastRelayCheck = lastSensorSend = lastWiFiCheck = lastEEPROMWrite = millis();
}

void loop() {
  unsigned long now = millis();

  if (now - lastWiFiCheck > 10000) {
    lastWiFiCheck = now;
    checkWiFi();
  }

  if (now - lastRelayCheck >= 5000) {
    lastRelayCheck = now;
    bool newR1 = fetchRelayCommand("ecs_1", "relay1", relayState1);
    bool newR2 = fetchRelayCommand("ecs_1", "relay2", relayState2);
    if (newR1 != relayState1 || newR2 != relayState2) {
      relayState1 = newR1;
      relayState2 = newR2;
      digitalWrite(RELAY1_PIN, relayState1 ? LOW : HIGH);
      digitalWrite(RELAY2_PIN, relayState2 ? LOW : HIGH);
      Serial.printf("🔄 Relays changed: R1=%s, R2=%s\n", relayState1 ? "ON" : "OFF", relayState2 ? "ON" : "OFF");
      float t1, t2, rh1, rh2;
      bool v1, v2;
      if (readSensors(t1, t2, rh1, rh2, v1, v2)) {
        sendSensorData("ecs_1", t1, t2, rh1, rh2, v1, v2, relayState1, relayState2);
      } else {
        sendSensorData("ecs_1", 0, 0, 0, 0, false, false, relayState1, relayState2);
      }
    }
  }

  if (now - lastSensorSend >= 40000) {
    lastSensorSend = now;
    float t1, t2, rh1, rh2;
    bool v1, v2;
    if (readSensors(t1, t2, rh1, rh2, v1, v2)) {
      sendSensorData("ecs_1", t1, t2, rh1, rh2, v1, v2, relayState1, relayState2);
    } else {
      sendSensorData("ecs_1", 0, 0, 0, 0, false, false, relayState1, relayState2);
    }
  }

  if (now - lastEEPROMWrite >= 10000) {
    lastEEPROMWrite = now;
    bool changed = false;
    if (EEPROM.read(EEPROM_RELAY1_ADDR) != (relayState1 ? 1 : 0)) {
      EEPROM.write(EEPROM_RELAY1_ADDR, relayState1 ? 1 : 0);
      changed = true;
    }
    if (EEPROM.read(EEPROM_RELAY2_ADDR) != (relayState2 ? 1 : 0)) {
      EEPROM.write(EEPROM_RELAY2_ADDR, relayState2 ? 1 : 0);
      changed = true;
    }
    if (changed) {
      EEPROM.commit();
      Serial.println("💾 Relay states saved to EEPROM.");
    }
  }

  delay(10);
}
