# ESP32 Token Counter Firmware


# ESP32 Token Counter Firmware

## Project Summary & Key Requirements

This firmware is designed for an ESP32 device to count tokens (e.g., meal coupons) for breakfast, lunch, and dinner, and report the counts to a Supabase backend. It is structured for easy merging with other codebases, including those with IR sensor integration.

### Key Features & Implementation
1. **WiFi Setup**: Uses WiFiManager for automatic WiFi connection and fallback portal. Device ID is used for identification in Supabase.
2. **Time Synchronization**: Uses NTP to synchronize the device clock at boot and periodically. Retries until a valid time is received before proceeding.
3. **Meal Time Logic**: Defines meal windows: Breakfast (7–9am), Lunch (11am–2pm), Dinner (6–9pm). Uses current IST time to determine which meal window is active. Macros (BFL, BFH, LFL, LFH, DFL, DFH) allow easy adjustment of meal windows. DUMMYHREFORTESTING macro allows simulation of different meal times for testing.
4. **Token Counting**: Maintains a TokenData struct with token_count, meal, date (yyyy-mm-dd IST), and update flag. On each loop, if within a meal window, increments the token count and sets the update flag. If not in a meal window, skips token increment and logs status.
5. **Persistent Storage (NVS)**: Uses ESP32 Preferences (NVS) for storing and restoring TokenData. On boot, loads token data from NVS and validates it. If the stored date and meal match the current values, keeps the count; otherwise, resets the count (unless not in a meal window). Data is saved to NVS every 20 seconds or when updated.
6. **Supabase Integration**: Sends token count updates to Supabase using HTTP PATCH requests. Only the relevant meal column (breakfast, lunch, or dinner) is updated in the payload. Filters by sensor_id and date in the request.
7. **Debug Logging**: Prints detailed logs for WiFi status, time sync, token data, and Supabase requests. Logs when not in a meal window, including current hour, token count, date, and meal.
8. **Testing Support**: DUMMYHREFORTESTING macro to simulate different meal times for testing.
9. **Code Cleanups**: All legacy schedule, timer, relay, and EEPROM logic has been removed. All persistent storage is now handled by NVS (Preferences).

### Key Discussion & Requirements
- Robust time sync and validation to ensure correct daily/meal token counting.
- Persistent storage using NVS for increased write lifecycle and reliability over EEPROM.
- Easy integration point for IR sensor logic: Replace the dummy token increment with actual IR sensor read logic.
- Defensive checks for corrupted or invalid NVS data.
- Only initialize/reset token count if current time is a valid meal window.
- All code is modular and ready for merging with IR sensor code or other extensions.

### TODOs for Integration
- Implement IRsensor_read() to increment token_count based on actual IR sensor events.
- Remove unused legacy code (OneWire, DallasTemperature, relay logic) if not needed for your hardware.
- Optionally add Prometheus metrics sending if required for your backend.

---

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
