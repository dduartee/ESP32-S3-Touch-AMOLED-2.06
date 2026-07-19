# Review: 09_basic_watch

## Summary

The implementation is a clean, focused deep sleep boot example that faithfully follows PLAN.md. It demonstrates the full boot→init→status→sleep→wakeup cycle using GPIO0 (BOOT button) as the wakeup source and the AXP2101 PMIC for battery monitoring via the ESP Component Registry version of XPowersLib. The code closely mirrors the I2C and PMU patterns from `01_AXP2101`, uses NVS correctly for a boot counter, and properly configures GPIO wakeup for ESP32-S3. The implementation is well-structured, minimal, and correct for its intended purpose.

## Plan Compliance

| PLAN.md Item | Status | Notes |
|---|---|---|
| Read wakeup cause via `esp_sleep_get_wakeup_cause()` | ✅ Done | Line 118 |
| Increment NVS boot counter | ✅ Done | Lines 121-140 |
| Init I2C bus (SCL=GPIO14, SDA=GPIO15) | ✅ Done | Lines 80-110 |
| Init AXP2101 via XPowersLib | ✅ Done | Lines 145-171 |
| Read battery voltage/percentage | ✅ Done | Lines 157-167 |
| Log status to console | ✅ Done | Throughout app_main |
| Configure GPIO0 wakeup (low-level) | ✅ Done | Lines 175-186 |
| Enter deep sleep | ✅ Done | Line 192 |
| Minimal mode (no display) | ✅ Done | Console-only output |
| `cube32esp/xpowerslib ^0.3.3` from registry | ✅ Done | idf_component.yml |
| Remove LVGL/audio from CMakeLists | ✅ Done | Single main.c source |
| Strip LVGL configs from sdkconfig | ✅ Done | No CONFIG_LV_* entries |
| Remove SPIRAM from sdkconfig | ✅ Done | No CONFIG_SPIRAM* entries |
| Keep flash/partition/Freertos configs | ✅ Done | Lines 4-11 |
| Partitions.csv unchanged (NVS present) | ✅ Done | NVS partition at 0x9000 |
| Display mode deferred to future | ✅ Acknowledged | Per PLAN.md section |

**Result: Fully compliant with PLAN.md.**

## Reference Comparison (01_AXP2101)

### I2C Init Pattern
09's I2C init (`main/main.c:80-110`) matches the reference (`01_AXP2101/main/main.cpp:39-69`) nearly identically: same bus config struct, same flags, same device address. 09 uses `AXP2101_SLAVE_ADDRESS` from XPowersLib.h while the reference hardcodes `0x34` — 09's approach is better (avoids magic number).

### PMU Read/Write Callbacks
`main/main.c:53-78` vs `01_AXP2101/main/main.cpp:72-96` — functionally identical. Both use `i2c_master_transmit_receive` / `i2c_master_transmit` with the same buffer allocation pattern. 09 uses `static` (correct for internal linkage in C); the reference uses extern linkage (needed for cross-file calls in C++).

### PMU.begin() Usage
Both call `PMU.begin(AXP2101_SLAVE_ADDRESS, read_fn, write_fn)`. The reference (`port_axp2101.cpp:18`) checks the return and returns `ESP_FAIL` on failure. 09 (`main/main.c:146`) logs an error but continues execution — see Issue #1 below.

### PMU Configuration Sequence
- Both call `disableTSPinMeasure()` first — critical for boards without battery temp sensor
- Both enable the same voltage/temperature measurements
- Reference additionally configures charging parameters (precharge, constant current, termination current, charge voltage) — 09 omits this, which is acceptable for a minimal deep sleep demo
- Reference enables specific IRQs (battery insert/remove, VBUS, power key, charge events); 09 disables all IRQs and doesn't re-enable — acceptable since 09 doesn't handle interrupts

### Key Difference
01_AXP2101 uses a local `components/XPowersLib/` copy; 09 uses the registry version via `idf_component.yml`. The API surface used is identical.

## Deep Sleep Verification

### GPIO Wakeup API (ESP32-S3)
```c
esp_sleep_enable_gpio_wakeup();                          // line 175
gpio_wakeup_enable(WAKEUP_GPIO, GPIO_INTR_LOW_LEVEL);   // line 186
```
This is the correct ESP-IDF API for ESP32-S3 deep sleep GPIO wakeup. The two-step pattern (enable the GPIO wakeup subsystem, then configure the specific GPIO) is correct. `ESP_SLEEP_WAKEUP_GPIO` (value 0x6) is the correct wake cause returned.

### GPIO Configuration
```c
gpio_config_t gpio_cfg = {
    .pin_bit_mask = BIT64(WAKEUP_GPIO),
    .mode = GPIO_MODE_INPUT,
    .pull_up_en = GPIO_PULLUP_ENABLE,
    .pull_down_en = GPIO_PULLDOWN_DISABLE,
    .intr_type = GPIO_INTR_LOW_LEVEL,
};
```
Correctly configures GPIO0 as input with internal pull-up before deep sleep. The pull-up ensures the pin stays high when the BOOT button is not pressed, preventing spurious wakeups.

### Pre-sleep Hygiene
`fflush(stdout)` at line 191 ensures all pending log output is flushed before deep sleep — good practice.

**Result: Deep sleep GPIO wakeup configuration is correct for ESP32-S3.**

## NVS Usage

- `nvs_flash_init()` with erase-and-retry on version mismatch (lines 121-126) — standard ESP-IDF pattern
- Boot counter stored as `int32_t` in `"storage"` namespace under key `"boot_count"` — correct
- `nvs_get_i32` returns `ESP_ERR_NVS_NOT_FOUND` on first boot, leaving `boot_count` at 0 before increment to 1 — correct
- `nvs_commit()` called after set — ensures durability
- `nvs_close()` called after operations complete — correct resource management

**Result: NVS usage is correct.**

## Issues Found

### Medium

**1. No error handling on AXP2101 init failure** (`main/main.c:146-148`)
```c
if (!PMU.begin(AXP2101_SLAVE_ADDRESS, pmu_register_read, pmu_register_write_byte)) {
    ESP_LOGE(TAG, "AXP2101 init failed");
} else {
```
If `PMU.begin()` fails, the code logs an error but continues to the deep sleep configuration. This means the device will enter deep sleep without any battery status being logged, and the user may not notice the failure in the logs. The reference (`port_axp2101.cpp:24-25`) returns `ESP_FAIL` on failure, which would be caught by `ESP_ERROR_CHECK` in `app_main`.

**Recommendation**: Either `ESP_ERROR_CHECK(ESP_FAIL)` / `abort()` on PMU init failure, or at minimum skip the battery reading block and log a warning that status is unavailable. Silent continuation could mask hardware wiring issues.

**2. `intr_type = GPIO_INTR_LOW_LEVEL` may be unnecessary** (`main/main.c:182`)
The `gpio_config()` call sets `intr_type = GPIO_INTR_LOW_LEVEL`, but this example doesn't use GPIO interrupts (no ISR, no event queue). The wakeup is configured via `gpio_wakeup_enable()` which operates independently of the GPIO interrupt controller. This field is harmless but misleading — it implies interrupt-driven handling that doesn't exist.

**Recommendation**: Either remove the `intr_type` field (set to `GPIO_INTR_DISABLE`) or add a comment explaining it's configured for consistency with the wakeup level.

### Low

**3. BSP dependency retained but unused** (`main/idf_component.yml:3-4`)
The `waveshare/esp32_s3_touch_amoled_2_06` BSP dependency is kept in `idf_component.yml` but no BSP APIs are used in `main.c`. PLAN.md notes this is "for potential future display use," but it adds build time and component size for no functional benefit in this example.

**Recommendation**: Consider removing the BSP dependency for the minimal example. It can be re-added when display mode is implemented. This would make the example truly self-contained and reduce build dependencies.

**4. `CONFIG_IDF_EXPERIMENTAL_FEATURES=y` present** (`sdkconfig.defaults:12`)
This config is not mentioned in PLAN.md's sdkconfig spec. While it may be required by a component, it should be documented if intentional.

**Recommendation**: Add a comment in sdkconfig.defaults noting why this is needed, or remove if not required.

**5. Charging not configured** (`main/main.c:145-171`)
The AXP2101 initialization enables voltage measurement but doesn't configure charging parameters (precharge current, constant current limit, termination current, charge voltage). The reference example (`port_axp2101.cpp:130-137`) explicitly sets these. While the AXP2101 has sensible defaults, this is a missed opportunity to demonstrate complete PMIC setup in a deep sleep context where charging-during-sleep is a key feature (noted in PLAN.md risk #7).

**Recommendation**: Add charging configuration to match the reference pattern, or add a comment noting that AXP2101 defaults are being used intentionally.

## Recommendations

1. **Add PMU init failure handling** — The most important fix. Silent failure on hardware init is a debugging trap.
2. **Remove unused `intr_type` or document it** — Minor clarity improvement.
3. **Remove BSP dependency** — Makes the example truly minimal and self-contained.
4. **Add charging configuration** — Completes the PMIC setup and aligns with the reference.
5. **Document `CONFIG_IDF_EXPERIMENTAL_FEATURES`** — If required, note why.

## Verdict

**PASS_WITH_NOTES**

The implementation is correct, well-structured, and fully compliant with PLAN.md. The deep sleep cycle works as designed. The issues found are all medium or low severity — no critical bugs or security concerns. Issue #1 (PMU init failure handling) is the most worth addressing before considering this example complete, as silent hardware failure is a common embedded debugging pain point.
