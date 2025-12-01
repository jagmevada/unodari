# ESP32 Token Counter Firmware

## Overview
This project is an ESP32-based token counter for meal management (breakfast, lunch, dinner) with persistent storage and cloud reporting.

## Features
- WiFi setup using WiFiManager (auto-connect and fallback portal)
- NTP time synchronization with retry logic
- Meal time logic:
  - Breakfast: 7–9am
  - Lunch: 11am–2pm
  - Dinner: 6–9pm
- Token counting per meal and day
- Persistent storage using ESP32 NVS (Preferences)
- Robust validation and initialization of token data
- Supabase integration (PATCH requests for meal columns)
- Debug logging for WiFi, time, token data, and Supabase requests
- Testing support via DUMMYHREFORTESTING macro
- All legacy schedule, timer, relay, and EEPROM logic removed

## Usage
1. Flash the firmware to your ESP32 device.
2. On first boot, connect to the WiFiManager portal to set up WiFi.
3. Device will sync time via NTP and begin counting tokens for each meal period.
4. Token counts are persisted in NVS and reported to Supabase.

## Customization
- Adjust meal time windows in `main.cpp` via macros (`BFL`, `BFH`, `LFL`, `LFH`, `DFL`, `DFH`).
- Use `DUMMYHREFORTESTING` to simulate different meal times for testing.
- Integrate IR sensor logic for real token detection (stub provided).

## TODO
- Implement IR sensor logic for token increment
- Add Prometheus metrics sending if required
- Remove unused legacy code if not needed

## License
MIT
