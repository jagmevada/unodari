/*
  Project overview for Copilot:

  Hardware:
  - MCU: ESP32
  - 4x push buttons used as a simple keypad:
      KEY1_PIN = 5
      KEY2_PIN = 17
      KEY3_PIN = 19
      KEY4_PIN = 18
    Buttons are wired as active-LOW with INPUT_PULLUP and use external interrupts.

  - 2x TCRT5000 reflective IR sensors:
      Sensor 1:
        Digital output -> S1_D0_PIN = 23
        Analog output  -> S1_A0_PIN = 32
      Sensor 2:
        Digital output -> S2_D0_PIN = 16
        Analog output  -> S2_A0_PIN = 33
    Analog is read with 12-bit resolution (0..4095) using analogReadResolution(12).

  - OLED display:
      128x64 SH1106 over I2C
      Using U8g2 full-buffer driver:
        U8G2_SH1106_128X64_NONAME_F_HW_I2C u8g2(U8G2_R0, U8X8_PIN_NONE);
      Default ESP32 I2C pins: SDA = 21, SCL = 22.

  Current software structure:

  - setup():
      - setupSerial(): Serial at 460800 baud.
      - setupDisplay(): u8g2.begin() and set font (base font configured).
      - setupSensors(): configure TCRT5000 digital pins as INPUT and set ADC resolution.
      - setupKeypad(): configure button pins as INPUT_PULLUP and attach FALLING-edge interrupts.

  - loop():
      - readSensors():
          * Reads both analog and digital values for S1 and S2.
          * Prints structured sensor data over Serial using a custom format
            intended for a VS Code Serial Plotter plugin (Mario Zechner).
            Example fields printed: "S1:<analog>, D1:<digital>, S2:<analog>, D2:<digital>".
      - handleKeypad():
          * Uses a generic processKey() helper.
          * ISRs only set volatile flags (g_keyXInterrupt).
          * processKey() does:
              - Check interrupt flag
              - Software debounce using BUTTON_DEBOUNCE_MS and millis()
              - Confirm button is still LOW
              - Call the corresponding onKeyXPressed() handler.
          * onKeyXPressed() updates g_lastKeyPressed ("Key 1", "Key 2", etc.)
            and logs to Serial.
      - updateDisplay():
          * Clears the U8g2 buffer, calls drawScreen(), then sends the buffer.

  - drawScreen():
      - Uses a big font (u8g2_font_10x20_tf) for the title "Counter".
      - Shows a 5-segment battery icon in the top-right corner (0–4 bars).
      - Uses a smaller bold font (u8g2_font_8x13B_tf) for data:
          * S1 A:<analog> D:<H/L>
          * S2 A:<analog> D:<H/L>
          * Last key: <string>

  Design notes:
  - Interrupts are IRAM_ATTR and kept minimal (only set flags).
  - Debouncing and logic are done in the main loop using processKey().
  - Serial output is structured for an Arduino-like / VS Code serial plotter.
  - The display code is separated into updateDisplay() + drawScreen() for clarity.

  Battery UI:
  - g_batteryLevelIndex is a 0..4 value corresponding to:
      0 -> 0%
      1 -> 25%
      2 -> 50%
      3 -> 75%
      4 -> 100%
  - drawBattery() draws a 5-bar icon at the top-right of the SH1106 screen.
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
uint8_t g_batteryLevelIndex = 4;  // start showing 100%

// =============================
// OLED Display (U8g2)
// =============================

// Full-frame buffer, SH1106 128x64, hardware I2C, no reset pin
// Uses default SDA=21, SCL=22 on ESP32
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

  // TODO: later you can update g_batteryLevelIndex based on ADC/battery
  // For now it's fixed.

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
  // Initialize I2C and display
  u8g2.begin();

  // Default font (we switch fonts inside drawScreen())
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
// Button Press Handlers
// =============================

void onKey1Pressed() {
  g_lastKeyPressed = "Key 1";
  g_batteryLevelIndex = 0;
  Serial.println("Key 1 pressed");
}

void onKey2Pressed() {
  g_lastKeyPressed = "Key 2";
  g_batteryLevelIndex = 1;
  Serial.println("Key 2 pressed");
}

void onKey3Pressed() {
  g_lastKeyPressed = "Key 3";
  g_batteryLevelIndex = 2;
  Serial.println("Key 3 pressed");
}

void onKey4Pressed() {
  g_lastKeyPressed = "Key 4";
  g_batteryLevelIndex = 3;
  Serial.println("Key 4 pressed");
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

  // Max 5 bars fit in this width
  for (uint8_t i = 0; i <= levelIndex; i++) {
    uint8_t bx = innerX + i * (barW + gap);
    u8g2.drawBox(bx, innerY, barW, innerH);
  }
}

void drawScreen() {
  const int16_t x0 = 0;
  int16_t y = 14;

  // Header: big font
  u8g2.setFont(u8g2_font_10x20_tf);
  u8g2.setCursor(x0, y);
  u8g2.print("Counter");

  // Battery icon on top-right
  drawBattery(g_batteryLevelIndex);

  // Move down for data lines
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
