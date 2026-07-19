# PLAN: 08_rtc_clock — PCF85063 RTC Clock with LVGL Display

## 1. Objective

Demonstrate reading time from the on-board PCF85063 RTC over I2C and displaying a live clock (HH:MM:SS + date) on the AMOLED screen using LVGL v9.

## 2. Hardware Used

| Peripheral | Interface | Pins | Notes |
|---|---|---|---|
| PCF85063 RTC | I2C | SDA=GPIO15, SCL=GPIO14 | Address 0x51, shared bus with CST9220 touch |
| CO5300 AMOLED | QSPI | CS=12, SCK=11, SDIO0-3=4-7 | 410x502, managed by BSP |
| CST9220 Touch | I2C | Same bus as RTC | Managed by BSP |
| AXP2101 PMIC | I2C | Same bus | Managed by BSP |

All I2C peripherals share one bus. The BSP initializes I2C and exposes the bus handle via `bsp_i2c_get_handle()`.

## 3. Library Choice

**Decision: Write a minimal C driver (~120 lines).**

### Rationale

| Option | Pros | Cons |
|---|---|---|
| `espp/pcf85063` (ESP Component Registry) | Full-featured, maintained | C++ library; pulls in `espp/base_peripheral` + `espp/utils` deps; heavier for a simple example |
| Minimal C driver (from Arduino SensorLib register map) | Zero extra deps, educational, fits the C codebase | Must implement BCD conversion ourselves |

The Arduino SensorLib driver (`SensorPCF85063.hpp`) reveals the PCF85063 protocol is trivial:
- I2C address: `0x51`
- 7 time registers at `0x04`-`0x0A` (seconds through year), all BCD-encoded
- Control registers at `0x00`-`0x01` (12/24h mode, clock enable)
- RAM register at `0x03` for device identification (bit 7 is R/W)
- Total: 3 I2C operations (init probe, read time, write time)

### Register Map (from `PCF85063Constants.h`)

```
0x00 CTRL1   - bit5: STOP, bit1: 12/24H select
0x01 CTRL2   - bit7: alarm enable, bit6: alarm flag
0x02 OFFSET  - frequency offset
0x03 RAM     - general purpose RAM (bit7: R/W test)
0x04 SEC     - seconds (bit7: osc invalid flag, bits6-0: BCD 00-59)
0x05 MIN     - minutes (bits6-0: BCD 00-59)
0x06 HOUR    - hours (bit5: AM/PM in 12h, bits4-0: BCD 00-23 or 00-12)
0x07 DAY     - day of month (bits5-0: BCD 01-31)
0x08 WEEKDAY - day of week (bits2-0: 0-6)
0x09 MONTH   - month (bits4-0: BCD 01-12)
0x0A YEAR    - year (BCD 00-99, represents 2000-2099)
0x0B-0x0F    - Alarm registers (not used in this example)
0x10-0x11    - Timer registers (not used in this example)
```

## 4. Architecture

```
┌─────────────┐     I2C      ┌──────────┐
│  app_main   │──────────────>│ PCF85063 │
│  (Core 1)   │              │   RTC    │
│             │              └──────────┘
│  ┌─────────┐│
│  │ RTC drv ││  Reads time every 1s
│  └────┬────┘│
│       │     │
│  ┌────▼────┐│     LVGL      ┌──────────┐
│  │ Display ││──────────────>│ CO5300   │
│  │  Task   ││              │ AMOLED   │
│  └─────────┘│              └──────────┘
└─────────────┘
```

### Task Structure

| Task | Core | Priority | Purpose |
|---|---|---|---|
| `app_main` | Core 1 | 5 | BSP init, I2C setup, LVGL UI setup |
| LVGL tick timer | Core 1 | 5 | `esp_timer` callback for `lv_tick_inc()` |
| LVGL task handler | Core 1 | 5 | Runs in `app_main` loop via `bsp_display` lock |

No separate RTC polling task needed — read RTC once per second in the LVGL timer callback or a periodic `esp_timer`.

### Time Synchronization

- On boot: set RTC to compile-time default (or last saved time from NVS)
- Every 1 second: read RTC via I2C, update LVGL labels
- NVS: optionally save time when user sets it (future enhancement, not in MVP)

## 5. RTC Configuration

| Parameter | Value |
|---|---|
| I2C Address | `0x51` |
| I2C Bus | BSP I2C bus (`bsp_i2c_get_handle()`) |
| I2C Speed | Default from BSP (100 kHz or 400 kHz) |
| Time Format | 24-hour mode (default, set in `initImpl`) |
| Date/Time Init | Compile-time constant: `2026-07-17 12:00:00` |
| Alarm | Not used in MVP (can be added later) |
| Oscillator | Starts automatically when clock is enabled (bit5 of CTRL1 = 0) |

### Init Sequence

1. Get I2C bus handle from BSP
2. Read RAM register (0x03), write bit7, verify R/W — confirms PCF85063 (not PCF8563)
3. Clear CTRL1 bit5 (STOP) to start the clock
4. Verify bit5 is clear (clock running)
5. Set 24-hour mode (clear CTRL1 bit1)
6. Write compile-time date/time to registers 0x04-0x0A

## 6. LVGL UI Design

### Layout (410x502 AMOLED)

```
┌──────────────────────────────┐
│                              │
│      ┌──────────────┐        │
│      │   12:34:56   │  HH:MM:SS in Montserrat 48
│      └──────────────┘        │
│                              │
│      ┌──────────────┐        │
│      │  Thu Jul 17  │  Date in Montserrat 24
│      │     2026     │
│      └──────────────┘        │
│                              │
│      ┌──────────────┐        │
│      │  PCF85063    │  Status label in Montserrat 16
│      │  RTC Clock   │
│      └──────────────┘        │
│                              │
│   ┌──────────────────────┐   │
│   │   Set Time (Boot)    │   │  Info text
│   └──────────────────────┘   │
│                              │
└──────────────────────────────┘
```

### Update Frequency

- **1 Hz** (every 1000ms): Read RTC, update time label `HH:MM:SS`
- Date label: Updated once at boot (or on midnight rollover)

### Fonts

- `lv_font_montserrat_48` — Time display (already enabled in sdkconfig)
- `lv_font_montserrat_24` — Date display
- `lv_font_montserrat_16` — Status/info text

All available via `CONFIG_LV_FONT_MONTSERRAT_*` in sdkconfig.defaults.

## 7. Files to Modify

### `main/main.c` — Full rewrite (26 lines → ~200 lines)

| Section | Change |
|---|---|
| Includes | Add `driver/i2c_master.h`, `<time.h>`, `nvs.h` |
| RTC driver | Add `pcf85063.h` and `pcf85063.c` as local source |
| `app_main` | Replace `lv_demo_music()` with RTC init + LVGL clock UI |
| LVGL UI | Create clock labels (time, date, status), center them |
| Timer | Add `esp_timer` at 1Hz to poll RTC and update labels |

### `main/pcf85063.h` — New file (~60 lines)

Minimal C driver header:
- `pcf85063_config_t` struct (I2C bus handle, address)
- `pcf85063_init()` — probe device, set 24h mode, start clock
- `pcf85063_set_time()` — write date/time to RTC registers
- `pcf85063_get_time()` — read date/time from RTC registers
- `pcf85063_time_t` struct (year, month, day, hour, minute, second, weekday)

### `main/pcf85063.c` — New file (~120 lines)

Implementation using ESP-IDF `i2c_master_*` API:
- BCD encode/decode helpers (`dec2bcd`, `bcd2dec`)
- `pcfh85063_write_reg()` / `pcf85063_read_reg()` — single/burst register I/O
- Day-of-week calculation (Zeller's congruence or lookup table)
- Register map constants from `PCF85063Constants.h`

### `main/CMakeLists.txt` — Add new source files

```cmake
idf_component_register(
    SRCS main.c pcf85063.c ${LV_DEMOS_SOURCES}
    INCLUDE_DIRS . ${LV_DEMO_DIR})
```

Remove LVGL demo sources (no longer needed for music demo).

### `main/idf_component.yml` — No changes needed

BSP already provides I2C access. No new component dependencies.

### `sdkconfig.defaults` — Minor changes

| Remove | Reason |
|---|---|
| `CONFIG_LV_USE_DEMO_MUSIC=y` | No longer using music demo |
| `CONFIG_LV_DEMO_MUSIC_AUTO_PLAY=y` | Same |
| `CONFIG_LV_USE_DEMO_WIDGETS=y` | No demo needed |
| `CONFIG_LV_USE_DEMO_BENCHMARK=y` | No demo needed |
| `CONFIG_LV_USE_DEMO_RENDER=y` | No demo needed |
| `CONFIG_LV_USE_DEMO_SCROLL=y` | No demo needed |
| `CONFIG_LV_USE_DEMO_STRESS=y` | No demo needed |
| `CONFIG_LV_USE_DEMO_TRANSFORM=y` | No demo needed |
| `CONFIG_LV_USE_DEMO_FLEX_LAYOUT=y` | No demo needed |
| `CONFIG_LV_USE_DEMO_MULTILANG=y` | No demo needed |

Keep font configs, LVGL OS, draw units, etc.

## 8. Dependencies

### `idf_component.yml` — Unchanged

```yaml
dependencies:
  espressif/esp_codec_dev:
    version: "~1.5"
    public: true
  espressif/usb:
    version: "^1.4.1"
  waveshare/esp32_s3_touch_amoled_2_06:
    version: "*"
  lvgl/lvgl:
    version: "9.5.0"
    public: true
```

No new component dependencies. The PCF85063 driver is written locally.

### `CMakeLists.txt` (project root) — Unchanged

Already correctly configured.

## 9. Risk / Notes

| Risk | Mitigation |
|---|---|
| I2C bus contention (RTC + touch + PMIC on same bus) | BSP already manages bus; use `i2c_master_bus_rm_device` or sequential access. The RTC I2C transaction is ~20 bytes, <1ms. |
| RTC not keeping time (no battery) | Acceptable for example. Document that time resets on power loss. Could add NVS persistence later. |
| PCF85063 vs PCF8563 confusion | The init probe (RAM bit7 R/W test) distinguishes them. Log error if wrong chip detected. |
| LVGL font size on 410x502 screen | Montserrat 48 fits ~8 characters wide. "12:34:56" = 8 chars — fits. Date "Thu Jul 17" fits in Montserrat 24. |
| Compile-time time init | Use `__DATE__` and `__TIME__` macros for automatic compile-time initialization. |

## 10. Implementation Order

1. Create `pcf85063.h` and `pcf85063.c` (minimal I2C driver)
2. Update `main/CMakeLists.txt` to include new source
3. Rewrite `main/main.c` with RTC init + LVGL clock UI
4. Clean up `sdkconfig.defaults` (remove demo configs)
5. Build, flash, test on hardware
