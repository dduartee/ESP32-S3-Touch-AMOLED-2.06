# Plan: 09_basic_watch

## Objective

Demonstrate ESP32-S3 deep sleep with GPIO wakeup using the BOOT button (GPIO0).
The example shows the full cycle: boot → init AXP2101 → display status → enter deep sleep → wakeup on button press → repeat.

## Hardware Used

| Peripheral | Role | Notes |
|------------|------|-------|
| AXP2101 PMIC | Power management, battery monitoring | I2C bus 1, SCL=GPIO14, SDA=GPIO15, addr 0x34 |
| BOOT button (GPIO0) | Deep sleep wakeup source | Active-low, internal pull-up available |
| AMOLED display | Pre-sleep status (optional) | CO5300, 410x502, QSPI via BSP |

## Library Choice

| Library | Purpose | Why |
|---------|---------|-----|
| `cube32esp/xpowerslib ^0.3.3` | AXP2101 driver | Available on ESP Component Registry, same API as local XPowersLib |
| ESP-IDF native (`esp_sleep.h`) | Deep sleep control | Built-in, no external dependency |

**Decision**: Use the ESP Component Registry version of XPowersLib instead of the local copy. This avoids duplicating the component and keeps the example self-contained via `idf_component.yml`.

## Architecture

```
Boot / Wakeup
    │
    ├─ Read wakeup cause (esp_sleep_get_wakeup_cause())
    ├─ Increment boot counter in NVS
    ├─ Init I2C bus → AXP2101 PMIC
    ├─ Read battery voltage/percentage
    ├─ (Optional) Init display → show status screen
    ├─ Log status to console
    │
    ├─ Configure GPIO0 as wakeup source (low-level trigger)
    ├─ Optionally: disable unused PMIC rails
    └─ Enter deep sleep (esp_deep_sleep_start())
```

## Deep Sleep Configuration

### Wakeup Source

```c
#include "esp_sleep.h"

// GPIO0 (BOOT button) - active low, use internal pull-up
esp_sleep_enable_gpio_wakeup();
gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);
```

### AXP2101 Power State During Sleep

The AXP2101 retains its register state across deep sleep (it's external to the ESP32-S3). Key decisions:

- **Keep ON**: DC3 (ESP32-S3 VDD), battery charging circuit, VBUS detection
- **Turn OFF before sleep**: Display power rails (ALDO for display), any unused LDOs
- **Charging**: Continue charging while sleeping (default behavior)

### Estimated Current

- ESP32-S3 deep sleep: ~7-10 µA
- AXP2101 standby: ~20-50 µA (depends on enabled rails)
- Total with display off: ~30-60 µA typical

## Display Strategy

**Two modes (configurable via Kconfig/sdkconfig)**:

1. **Minimal mode** (default): No display, console-only output. Smallest binary, fastest boot-to-sleep.
2. **Display mode**: Show battery %, voltage, sleep count, wakeup reason using LVGL label. Brief 3-second display then auto-sleep.

For the initial implementation, use **minimal mode** (console only) to keep the example focused on deep sleep mechanics. Display support can be added as an optional layer.

## Files to Modify

### `main/main.c` — **Complete rewrite**

Remove LVGL music demo entirely. New structure:
- Read NVS boot counter and wakeup cause
- Init I2C + AXP2101
- Log battery status
- Configure GPIO0 wakeup
- Enter deep sleep

### `main/CMakeLists.txt` — **Simplify**

Remove LVGL demo source globbing. Keep only `main.c`:
```cmake
idf_component_register(SRCS main.c INCLUDE_DIRS ".")
```

### `main/idf_component.yml` — **Replace dependencies**

Remove LVGL and esp_codec_dev (not needed for minimal mode). Keep BSP and add XPowersLib:
```yaml
dependencies:
  waveshare/esp32_s3_touch_amoled_2_06:
    version: "*"
  cube32esp/xpowerslib:
    version: "^0.3.3"
```

### `sdkconfig.defaults` — **Strip LVGL configs**

Remove all `CONFIG_LV_*` lines. Keep flash/SPIRAM/base configs. Remove SPIRAM if not needed for this example (deep sleep doesn't use PSRAM).

### `CMakeLists.txt` — **No changes needed** (project name already set)

### `partitions.csv` — **Keep as-is** (NVS needed for boot counter)

## sdkconfig Changes

**Remove**:
- All `CONFIG_LV_*` entries (LVGL config)
- `CONFIG_SPIRAM*` entries (not needed, wastes power)
- `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` (use default 160MHz for lower power)

**Keep**:
- `CONFIG_ESPTOOLPY_FLASHMODE_QIO=y`
- `CONFIG_ESPTOOLPY_FLASHSIZE_16MB=y`
- `CONFIG_PARTITION_TABLE_CUSTOM=y`
- `CONFIG_COMPILER_OPTIMIZATION_PERF=y`
- `CONFIG_FREERTOS_HZ=1000`

**Add** (optional):
```
CONFIG_PMU_I2C_SCL=14
CONFIG_PMU_I2C_SDA=15
CONFIG_PMU_INTERRUPT_PIN=-1
```

## Dependencies (`idf_component.yml`)

```yaml
## IDF Component Manager Manifest File
dependencies:
  waveshare/esp32_s3_touch_amoled_2_06:
    version: "*"
  cube32esp/xpowerslib:
    version: "^0.3.3"
```

## Risk/Notes

1. **GPIO0 wakeup**: ESP32-S3 GPIO0 has an internal pull-up. `gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL)` works with the BOOT button (active-low, connects to GND when pressed). No external pull-up needed.

2. **AXP2101 state persistence**: The AXP2101 retains register configuration across ESP32-S3 deep sleep cycles. If you disable rails before sleep, you must re-enable them after wakeup. The PMIC itself does not reset.

3. **NVS boot counter**: Use NVS to persist a boot counter across deep sleep cycles. This proves wakeup is working and demonstrates NVS persistence.

4. **Wakeup cause**: `esp_sleep_get_wakeup_cause()` returns `ESP_SLEEP_WAKEUP_GPIO` on GPIO wakeup. On power-on reset, returns `ESP_SLEEP_WAKEUP_NONE`.

5. **Display re-init after wakeup**: If using display mode, the BSP/display must be fully re-initialized after each wakeup (deep sleep resets all peripherals).

6. **SPIRAM power**: SPIRAM draws current even in deep sleep if left enabled. Disable it in sdkconfig for minimal power consumption.

7. **Charging during sleep**: AXP2101 continues charging the battery while ESP32-S3 is in deep sleep. This is desirable behavior.
