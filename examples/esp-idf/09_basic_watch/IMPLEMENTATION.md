# Implementation: 09_basic_watch

## What Was Implemented

A minimal ESP-IDF example demonstrating the deep sleep → GPIO wakeup cycle
on the Waveshare ESP32-S3-Touch-AMOLED-2.06 board.

### Flow

1. ESP32-S3 boots (power-on or GPIO wakeup)
2. Wakeup cause is read via `esp_sleep_get_wakeup_cause()`
3. NVS boot counter is incremented (persists across deep sleep cycles)
4. I2C bus is initialized (SCL=GPIO14, SDA=GPIO15)
5. AXP2101 PMIC is initialized via `cube32esp/xpowerslib`
6. Battery voltage, percentage, VBUS status, system voltage, and charging
   state are logged to the console
7. GPIO0 (BOOT button) is configured as a low-level wakeup source with
   internal pull-up enabled
8. System enters deep sleep via `esp_deep_sleep_start()`
9. Pressing the BOOT button (GPIO0 → GND) wakes the system, repeating
   the cycle

## Key Design Decisions

- **Console-only, no display**: Keeps the example focused on deep sleep
  mechanics. Display re-init after wakeup is an optional extension.
- **`cube32esp/xpowerslib` from registry**: Avoids duplicating the local
  XPowersLib component. The library is fetched automatically via
  `idf_component.yml`.
- **NVS boot counter**: A simple `int32_t` in the `storage` NVS namespace
  proves wakeup is working and demonstrates NVS persistence across sleep
  cycles.
- **GPIO0 with internal pull-up**: The BOOT button is active-low (connects
  to GND when pressed). The internal pull-up keeps it high during sleep;
  pressing it pulls low to trigger wakeup.
- **No SPIRAM**: Deep sleep doesn't use PSRAM. Removing it from sdkconfig
  saves power.
- **No LVGL**: LVGL configs are stripped from sdkconfig. The BSP dependency
  is kept in `idf_component.yml` for potential future display use, but the
  build CMakeLists is simplified to compile only `main.c`.

## Build & Test

```bash
cd examples/esp-idf/09_deep_sleep_boot
idf.py set-target esp32s3
idf.py build
idf.py -p /dev/ttyACM0 flash monitor
```

Expected console output on first boot:

```
I (xxx) deep_sleep: ========================================
I (xxx) deep_sleep: Deep Sleep Boot Example
I (xxx) deep_sleep: ========================================
I (xxx) deep_sleep: Wakeup cause: UNDEFINED (0x0)
I (xxx) deep_sleep: Boot count: 1
I (xxx) deep_sleep: I2C initialized
I (xxx) deep_sleep: AXP2101 initialized successfully
I (xxx) deep_sleep: Battery: 3800 mV, 75%
I (xxx) deep_sleep: VBUS: present (5000 mV)
I (xxx) deep_sleep: System: 3300 mV
I (xxx) deep_sleep: Charging: no
I (xxx) deep_sleep: Configuring GPIO0 as wakeup source (low-level trigger)
I (xxx) deep_sleep: Entering deep sleep. Press BOOT (GPIO0) to wake up.
```

After pressing BOOT button (wakeup):

```
I (xxx) deep_sleep: Wakeup cause: GPIO (0x6)
I (xxx) deep_sleep: Boot count: 2
...
```

## Deviations from PLAN.md

None. Implementation follows the spec exactly.

## Files

| File | Status |
|------|--------|
| `main/main.c` | New, complete |
| `main/CMakeLists.txt` | Simplified (removed LVGL/audio) |
| `main/idf_component.yml` | Updated (removed LVGL/codec, added xpowerslib) |
| `sdkconfig.defaults` | Stripped (removed LVGL, SPIRAM) |
| `partitions.csv` | Unchanged |
| `IMPLEMENTATION.md` | This file |
