// #include <Adafruit_GFX.h>
// #include <Adafruit_SH1106.h>
// =============================
// Pin Definitions
// =============================

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
#define LOOP_DELAY_MS       50UL   // main loop delay

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

// =============================
// OLED Display (U8g2)
// =============================

// Full-frame buffer, SSD1306 128x64, hardware I2C, no reset pin
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
  Serial.begin(115200);
  delay(200);
  Serial.println();
  Serial.println("=== ESP32 Keypad + TCRT5000 + OLED (U8g2) ===");
}

void setupDisplay() {
  // Initialize I2C and display
  u8g2.begin();
  u8g2.setFont(u8g2_font_6x10_tf);  // similar to u8g_font_6x10
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

  // Debug print
  Serial.print("S1 A0: ");
  Serial.print(g_sensor1Analog);
  Serial.print("  D0: ");
  Serial.print(g_sensor1Digital ? "HIGH" : "LOW");

  Serial.print("  |  S2 A0: ");
  Serial.print(g_sensor2Analog);
  Serial.print("  D0: ");
  Serial.println(g_sensor2Digital ? "HIGH" : "LOW");
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
  Serial.println("Key 1 pressed");
}

void onKey2Pressed() {
  g_lastKeyPressed = "Key 2";
  Serial.println("Key 2 pressed");
}

void onKey3Pressed() {
  g_lastKeyPressed = "Key 3";
  Serial.println("Key 3 pressed");
}

void onKey4Pressed() {
  g_lastKeyPressed = "Key 4";
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

void drawScreen() {
  const int16_t x0    = 0;   // start from column 1 to compensate left shift
  const int16_t lineH = 10;
  int16_t y = 10;

  // Shorter title to fit in 128 px
  u8g2.setCursor(x0, y);
  u8g2.print("TCRT + Keypad");
  y += lineH;

  // Sensor 1
  u8g2.setCursor(x0, y);
  u8g2.print("S1 A:");
  u8g2.print(g_sensor1Analog);
  u8g2.print(" D:");
  u8g2.print(g_sensor1Digital ? "H" : "L");
  y += lineH;

  // Sensor 2
  u8g2.setCursor(x0, y);
  u8g2.print("S2 A:");
  u8g2.print(g_sensor2Analog);
  u8g2.print(" D:");
  u8g2.print(g_sensor2Digital ? "H" : "L");
  y += lineH;

  // Last key
  u8g2.setCursor(x0, y);
  u8g2.print("Last key: ");
  u8g2.print(g_lastKeyPressed);
  y += lineH;

  u8g2.setCursor(x0, y);
  u8g2.print("Press keys to test");
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
