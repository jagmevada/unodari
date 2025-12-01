/*
  Project overview for Copilot (ESP32 + TCRT + Keypad + OLED UI):

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
    - Left side: header text "Counter" in big font (u8g2_font_10x20_tf).
    - Right side: status icons:
        [ WiFi icon ] [ Battery icon ]

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

  Middle / bottom area:
    - Sensor/diagnostic text in smaller bold font (u8g2_font_8x13B_tf):
        Line 1: "S1 A:<analog> D:<H/L>"
        Line 2: "S2 A:<analog> D:<H/L>"
        Line 3: "Last key: <string>"

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
          - Prints structured values to Serial for use with
            the VS Code Serial Plotter plugin (Mario Zechner style).
      * handleKeypad()
          - Generic processKey() that:
              - Checks interrupt flags (set by ISRs).
              - Does software debouncing with BUTTON_DEBOUNCE_MS and millis().
              - Confirms button is still LOW before calling handler.
          - Handlers onKey1Pressed()..onKey4Pressed() update global state.
      * updateDisplay()
          - Clears U8g2 buffer, calls drawScreen(), then sendBuffer().

  - drawScreen():
      * Draws header "Counter".
      * Draws top-right status icons:
            drawBattery(g_batteryLevelIndex);
            drawWifi(g_wifiLevelIndex);
      * Draws sensor data + last key line.

  ==========================================
  UI state variables for backend integration
  ==========================================

  These are the two key globals that backend / WiFi / battery code
  should update. Copilot should use these when integrating real logic:

    - uint8_t g_batteryLevelIndex
        Range: 0..4
        Meaning:
          0 → 0% / empty
          1 → ~25%
          2 → ~50%
          3 → ~75%
          4 → ~100%
        Usage:
          - Backend code (e.g., reading a battery voltage via ADC)
            should quantize the real battery level and set this variable.
          - drawBattery() uses this index purely as a visual.

    - uint8_t g_wifiLevelIndex
        Range: 0..4
        Meaning:
          0 → "no network" (circle with slash)
          1 → 1 bar (weak)
          2 → 2 bars
          3 → 3 bars
          4 → 4 bars (full / strong)
        Usage:
          - Backend WiFi code should:
              * Set g_wifiLevelIndex = 0 when WiFi is disconnected
                or connection fails.
              * Set 1..4 based on RSSI thresholds from WiFi.RSSI().
                Example mapping (for Copilot implementation):
                  RSSI <= -80 dBm → 1
                  -80 < RSSI <= -70 → 2
                  -70 < RSSI <= -60 → 3
                  RSSI >  -60        → 4
          - drawWifi() only reads this index and renders the icon.

  ==========================================
  Time display (top bar clock for backend)
  ==========================================

  - A new global string:
        String g_timeString
        Example value: "12:45 PM"

  - drawScreen() prints this time string on the top row,
    to the left of the WiFi and Battery icons (status bar style).

  - Backend (NTP / RTC / WiFi time sync) should:
        * Format current time as "hh:mm AM/PM"
        * Assign it to g_timeString whenever time updates:
              g_timeString = formattedString;

  - drawScreen() treats g_timeString as view-only and does not
    attempt to compute time by itself.

  ==========================================
  Current demo behavior (for testing only)
  ==========================================

  For now, keypad presses are used ONLY to demo the WiFi icon:

    - Key 1 → g_wifiLevelIndex = 0 (no network symbol, circle with slash)
    - Key 2 → g_wifiLevelIndex = 1 (1 bar)
    - Key 3 → g_wifiLevelIndex = 3 (3 bars)
    - Key 4 → g_wifiLevelIndex = 4 (4 bars, full)

  Battery:
    - g_batteryLevelIndex is currently fixed at 4 (100%).
    - This is a placeholder; backend code should overwrite it
      when the real battery measurement logic is implemented.

  ==========================================
  Notes for future Copilot assistance
  ==========================================

  - Keep ISRs minimal (only set flags).
  - Keep debounce + logic inside handleKeypad()/processKey().
  - To integrate WiFi:
      * Add WiFi.begin() etc. in setup().
      * Periodically read WiFi.status() and WiFi.RSSI() in loop()
        or a dedicated function, then update g_wifiLevelIndex.
  - To integrate battery:
      * Add an ADC reading function that samples the battery input,
        applies scaling/filtering, then sets g_batteryLevelIndex.
  - To integrate time:
      * Add NTP/RTC sync and format "hh:mm AM/PM" into g_timeString.
  - drawBattery(), drawWifi(), and the time display should be treated
    as pure view functions that only read the corresponding global state.
*/

#include <Arduino.h>
#include <Wire.h>
#include <U8g2lib.h>

// =============================
// Pin Definitions
// =============================

// Keypad (4 buttons)
#define KEY1_PIN  5    // Button "1"
#define KEY2_PIN  17   // Button "2"
#define KEY3_PIN  19   // Button "3"
#define KEY4_PIN  18   // Button "4"

// TCRT5000 Sensor 1
#define S1_D0_PIN 23
#define S1_A0_PIN 32

// TCRT5000 Sensor 2
#define S2_D0_PIN 16
#define S2_A0_PIN 33

// I2C pins for ESP32 (OLED) - hardware default: SDA=21, SCL=22

// =============================
// Config Macros
// =============================

#define BUTTON_DEBOUNCE_MS  50UL   // debounce time for keypad (in ms)
#define LOOP_DELAY_MS       10UL   // main loop delay

// =============================
// Global State
// =============================

// Sensor readings
int  g_sensor1Analog = 0;
int  g_sensor2Analog = 0;
bool g_sensor1Digital = false;
bool g_sensor2Digital = false;

// Last pressed key info (for display)
String g_lastKeyPressed = "None";

// Interrupt flags for buttons (set in ISR, handled in loop)
volatile bool g_key1Interrupt = false;
volatile bool g_key2Interrupt = false;
volatile bool g_key3Interrupt = false;
volatile bool g_key4Interrupt = false;

// Debounce timestamps
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
String g_timeString = "12:00AM"; // TODO: backend should set real time here

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
  setupSerial();
  setupDisplay();
  setupSensors();
  setupKeypad();

  Serial.println("System initialized.");
}

// =============================
// Main Loop
// =============================

void loop() {
  readSensors();
  handleKeypad();
  updateDisplay();

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
// Sensor Handling
// =============================

void readSensors() {
  // Analog
  g_sensor1Analog = analogRead(S1_A0_PIN);
  g_sensor2Analog = analogRead(S2_A0_PIN);

  // Digital
  g_sensor1Digital = digitalRead(S1_D0_PIN);
  g_sensor2Digital = digitalRead(S2_D0_PIN);

  // Debug print  // using serial plotter vscode plugin by Mario Zechner
  Serial.print(">");

  Serial.print("S1: ");
  Serial.print(g_sensor1Analog);
  Serial.print(",");

  Serial.print("D1: ");
  Serial.print(g_sensor1Digital);
  Serial.print(",");

  Serial.print("S2: ");
  Serial.print(g_sensor2Analog);
  Serial.print(",");

  Serial.print("D2: ");
  Serial.println(g_sensor2Digital);
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
// Button Press Handlers (WiFi demo only)
// =============================

// Key 1 → no-network symbol (circle with slash)
void onKey1Pressed() {
  g_lastKeyPressed = "Key 1";
  g_wifiLevelIndex = 0;
  Serial.println("Key 1 pressed -> WiFi NO NETWORK (circle with slash)");
}

// Key 2 → 1 bar
void onKey2Pressed() {
  g_lastKeyPressed = "Key 2";
  g_wifiLevelIndex = 1;
  Serial.println("Key 2 pressed -> WiFi 1 bar");
}

// Key 3 → 3 bars
void onKey3Pressed() {
  g_lastKeyPressed = "Key 3";
  g_wifiLevelIndex = 3;
  Serial.println("Key 3 pressed -> WiFi 3 bars");
}

// Key 4 → 4 bars (full)
void onKey4Pressed() {
  g_lastKeyPressed = "Key 4";
  g_wifiLevelIndex = 4;
  Serial.println("Key 4 pressed -> WiFi 4 bars (full)");
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

  // Header: big font
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.setCursor(x0, y);
  u8g2.print("D");

  // ---- Draw Battery & WiFi first (defines right boundary for time) ----
  drawBattery(g_batteryLevelIndex);
  drawWifi(g_wifiLevelIndex);

  // ---- Time on SAME LINE as WiFi/Battery ----
  u8g2.setFont(u8g2_font_7x13B_tf);

  // WiFi icon bounding box values
  const uint8_t battW = 18;
  const uint8_t tipW  = 2;
  const uint8_t wifiW = 12;

  int16_t battX = 128 - battW - tipW - 1;
  int16_t wifiX = battX - wifiW - 3;   // 3px gap before battery

  // Time ends just before WiFi icon, with 3px safety gap
  int16_t timeWidth = u8g2.getStrWidth(g_timeString.c_str());
  int16_t timeX = wifiX - timeWidth - 3;
  if (timeX < 0) timeX = 0;

  // -3 aligns 7x13 font vertically with icon top row
  u8g2.setCursor(timeX, y - 3);
  u8g2.print(g_timeString);

  // ----- Move down for data lines -----
  y += 16;

  // Data: smaller bold font
  u8g2.setFont(u8g2_font_8x13B_tf);

  u8g2.setCursor(x0, y);
  u8g2.print("S1 A:");
  u8g2.print(g_sensor1Analog);
  u8g2.print(" D:");
  u8g2.print(g_sensor1Digital ? "H" : "L");
  y += 13;

  u8g2.setCursor(x0, y);
  u8g2.print("S2 A:");
  u8g2.print(g_sensor2Analog);
  u8g2.print(" D:");
  u8g2.print(g_sensor2Digital ? "H" : "L");
  y += 13;

  u8g2.setCursor(x0, y);
  u8g2.print("Last key: ");
  u8g2.print(g_lastKeyPressed);
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
