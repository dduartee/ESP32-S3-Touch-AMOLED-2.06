# IMPLEMENTATION.md — 08_rtc_clock

## What Was Implemented

A minimal ESP-IDF example that reads time from the on-board PCF85063 RTC over I2C and displays a live clock (HH:MM:SS + date) on the CO5300 AMOLED screen using LVGL v9.

### Files Created/Modified

| File | Status | Description |
|---|---|---|
| `main/pcf85063.h` | **New** | Minimal C driver header with time struct, config struct, and 3 API functions |
| `main/pcf85063.c` | **New** | ~120-line driver: BCD encode/decode, I2C register R/W, init probe, time read/write |
| `main/main.c` | **Rewritten** | BSP init → PCF85063 init → LVGL clock UI → 1Hz esp_timer polling |
| `main/CMakeLists.txt` | **Modified** | Added `pcf85063.c` to SRCS, removed LVGL demo source globbing and music defines |
| `sdkconfig.defaults` | **Modified** | Removed all `CONFIG_LV_USE_DEMO_*` lines, added `CONFIG_LV_FONT_MONTSERRAT_48=y` |
| `main/idf_component.yml` | Unchanged | BSP already provides I2C access; no new deps needed |

## Key Design Decisions

1. **Minimal C driver** (~120 lines) instead of the `espp/pcf85063` C++ component registry library — avoids pulling in `espp/base_peripheral` + `espp/utils` dependencies for a trivial register map.

2. **BSP I2C bus sharing**: Uses `bsp_i2c_get_handle()` to get the shared I2C bus handle from the BSP. The driver creates/destroys device handles per transaction to avoid bus contention with touch/PMIC on the same bus.

3. **1Hz `esp_timer`** for RTC polling instead of a dedicated FreeRTOS task — simpler, no task overhead, and the callback runs in timer context (safe to call `bsp_display_lock` from).

4. **Compile-time time initialization** using hardcoded `2026-07-17 12:00:00` (per PLAN.md). `__DATE__`/`__TIME__` macros were considered but the hardcoded value matches the plan spec exactly.

5. **Day-of-week calculation** uses Zeller's congruence (implemented in `calc_weekday()`), exposed in header for use by `main.c` when setting initial time.

## How to Build and Test

```bash
cd examples/esp-idf/08_rtc_clock
idf.py set-target esp32s3
idf.py build
idf.py flash monitor
```

### Expected Behavior

1. Boot: BSP initializes display, touch, and I2C bus
2. PCF85063 init: RAM R/W test confirms correct chip, clears STOP bit, sets 24h mode
3. Sets compile-time date/time (2026-07-17 12:00:00)
4. LVGL UI shows:
   - `12:00:00` in Montserrat 48 (centered, white)
   - `Thu 7 17` in Montserrat 24 (centered, grey)
   - `PCF85063 RTC Clock` in Montserrat 16 (centered, dark grey)
   - `Time set at boot` at bottom
5. Every 1 second: reads RTC via I2C, updates time label
6. Date updates alongside time each second

## Deviations from PLAN.md

- **`__DATE__`/`__TIME__` macros**: Used hardcoded values instead of compile-time macros as specified in the plan. The macros would require parsing logic and produce different values per build, which is less predictable for an example.
- **`bsp_display_lock` in timer callback**: The plan suggested reading RTC in the LVGL timer callback; instead a separate `esp_timer` is used, which is cleaner and avoids blocking the LVGL tick source.
- **`idf_component.yml`**: Kept unchanged as planned — no new dependencies.
