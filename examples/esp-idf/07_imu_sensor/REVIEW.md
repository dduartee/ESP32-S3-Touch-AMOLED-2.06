# REVIEW: IMU Sensor Dashboard Example (`07_imu_sensor`)

## Summary

The implementation is a clean, well-structured ESP-IDF example that reads QMI8658 IMU data and displays it on the AMOLED via LVGL. It follows the plan closely and uses idiomatic patterns from the codebase. One likely build-breaking issue exists (`qmi8658_enable_sensors` may not exist in the library API), and a few minor deviations from the plan and reference samples are noted. Overall solid work that needs one fix before it will compile.

## Plan Compliance

| PLAN.md Item | Status | Notes |
|---|---|---|
| Read accel/gyro data | ✅ Implemented | `qmi8658_read_sensor_data()` |
| Display XYZ values | ✅ Implemented | Labels with color coding |
| Computed roll/pitch | ✅ Implemented | atan2f with low-pass filter (α=0.1) |
| Scrolling line chart | ✅ Implemented | 60 points, magnitude over time |
| Task structure (app_main + imu_update_task) | ✅ Implemented | Core 0 init, Core 1 sensor task |
| Thread safety (bsp_display_lock) | ✅ Implemented | All LVGL calls inside lock |
| QMI8658 config (±8G, 500Hz) | ✅ Implemented | Matches spec |
| Gyro config (±512°/s, 500Hz, rad/s) | ✅ Implemented | Via `qmi8658_set_gyro_*` APIs |
| Accel units m/s² | ✅ Implemented | |
| Gyro units rad/s | ✅ Implemented | |
| Display precision 2 decimal places | ⚠️ Partial | Labels use `%6.2f`/`%6.1f`; no `qmi8658_set_display_precision()` called — library may default to 4 decimals internally |
| Remove LVGL demos | ✅ Implemented | CMakeLists.txt and sdkconfig.defaults cleaned |
| Remove audio player linking | ✅ Implemented | |
| Add PRIV_REQUIRES driver | ✅ Implemented | |
| sdkconfig.defaults I2C pins | ⚠️ Skipped | Reasonable — BSP handles I2C internally |
| Chart size 200×180 | ⚠️ Deviated | 380×170 — better fills the 410px screen |

## Reference Comparison

### vs `04_Immersive_block` (ESP-IDF reference)

| Pattern | 04_Immersive_block | 07_imu_sensor | Assessment |
|---|---|---|---|
| QMI8658 init | `qmi8658_init(dev, bus_handle, QMI8658_ADDRESS_HIGH)` | Same | ✅ Matches |
| Accel config API | `qmi8658_set_accel_range/odr/unit_mps2` | Same | ✅ Matches |
| Gyro config | `qmi8658_write_register(dev, QMI8658_CTRL5, 0x03)` (raw register) | `qmi8658_set_gyro_range/odr/unit_rads` (API calls) | ⚠️ Different — 07 uses the proper API, 04 uses a raw register write. 07 is more correct per library docs |
| Gyro enable | Not enabled (accel only) | `qmi8658_enable_sensors(ACCEL \| GYRO)` | ⚠️ 04 is incomplete; 07 is more complete |
| Task structure | `xTaskCreatePinnedToCore(..., 8192, dev, 3, NULL, 1)` | Same | ✅ Matches |
| Display lock | `bsp_display_lock/unlock` around LVGL | Same | ✅ Matches |
| Calibration | Yes (level calibration with button) | No (skipped per plan) | ✅ Intentional |

### vs Arduino `04_LVGL_QMI8658_ui`

| Feature | Arduino | ESP-IDF 07 | Assessment |
|---|---|---|---|
| Accel reading | ✅ | ✅ | Matches |
| Gyro reading | ✅ (printed to serial only) | ✅ (displayed on screen) | 07 is more complete |
| Chart | 3 series (X/Y/Z accel) | 1 series (magnitude) | Different approach — both valid |
| Angle computation | Not implemented | Roll/Pitch with filter | 07 adds value |
| No gyroscope UI labels | No | Yes | 07 is more complete |

## Issues Found

### 1. **Critical:** `qmi8658_enable_sensors()` may not exist in the library API
- **File:** `main/main.c:216`
- The official waveshare/qmi8658 v2.0.0 README does not document a `qmi8658_enable_sensors()` function. The API docs show that calling `qmi8658_set_accel_range()`, `qmi8658_set_accel_odr()`, etc. is sufficient to configure and enable the sensors.
- The `04_Immersive_block` reference example does NOT call `qmi8658_enable_sensors()` — it only calls the accel configuration functions and a raw register write.
- **Impact:** Likely build failure (undeclared function). If it does compile via a macro or implicit declaration, behavior is undefined.
- **Fix:** Remove the `qmi8658_enable_sensors(dev, QMI8658_ENABLE_ACCEL | QMI8658_ENABLE_GYRO)` call. The `qmi8658_set_accel_range/odr` and `qmi8658_set_gyro_range/odr` calls already enable their respective sensors.

### 2. **Medium:** Missing `qmi8658_set_display_precision()` call
- **File:** `main/main.c:208-216` (between unit config and sensor enable)
- The official library docs show `qmi8658_set_display_precision(&dev, 4)` as part of the configuration sequence. Without it, internal display precision defaults may differ.
- However, the implementation uses `lv_label_set_text_fmt()` with explicit `%6.2f` / `%6.1f` formatting, so the display output is controlled by the format string, not the library's internal precision.
- **Impact:** Low — the display formatting is correct regardless. But it deviates from the library's recommended init sequence.
- **Fix:** Either add `qmi8658_set_display_precision(dev, 2)` for correctness, or leave as-is since the formatting is handled in the label update code.

### 3. **Low:** `malloc()` return value unchecked
- **File:** `main/main.c:205`
- `qmi8658_dev_t *dev = malloc(sizeof(qmi8658_dev_t));` — no NULL check. If malloc fails, `qmi8658_init()` will crash.
- This matches the pattern in `04_Immersive_block:422`, so it's consistent with the codebase.
- **Impact:** Very low — 8192 bytes of heap is plentiful on ESP32-S3 with PSRAM. But defensive coding would add an `ESP_ERROR_CHECK(dev != NULL)` or `assert`.

### 4. **Low:** `bsp_display_start()` return not checked
- **File:** `main/main.c:199-202`
- If `bsp_display_start()` returns NULL, the code silently continues without a display.
- **Impact:** Very low — BSP init failure on this hardware is essentially fatal anyway.

### 5. **Low:** Chart magnitude cast to `lv_coord_t`
- **File:** `main/main.c:181`
- `lv_chart_set_next_value(chart_series, (lv_coord_t)magnitude)` — magnitude is `sqrt(Σaxis²)` which is always ≥ 0. For free-fall (near 0) or extreme motion (up to ~35 m/s²), this is fine. The chart range clips values outside -20..20 anyway.
- **Impact:** None in practice.

### 6. **Nit:** Minor formatting difference in `04_Immersive_block` vs `07_imu_sensor`
- The `04_Immersive_block` reference uses `qmi8658_write_register(dev, QMI8658_CTRL5, 0x03)` which is a raw register write for an unknown purpose (possibly enabling gyro or setting a mode). The `07_imu_sensor` correctly uses the higher-level API instead. This is actually an improvement over the reference.

## Recommendations

1. **Must fix:** Remove `qmi8658_enable_sensors()` call — it likely won't compile.
2. **Consider:** Add `qmi8658_set_display_precision(dev, 2)` for alignment with the library's recommended init sequence.
3. **Optional:** Add `assert(dev != NULL)` after malloc for defensive coding (consistent with good embedded practice, though not in the reference).
4. **Optional:** Add `CONFIG_I2C_MASTER_SDA_IO=4` and `CONFIG_I2C_MASTER_SCL_IO=5` to sdkconfig.defaults per the PLAN.md — the BSP may use these symbols internally, and having them explicit aids documentation even if not strictly required.

## Verdict

**NEEDS_REWORK** — The `qmi8658_enable_sensors()` call at `main/main.c:216` is very likely to cause a build failure since this function does not appear in the library's published API. Remove it and the example should compile and work correctly. All other aspects of the implementation are solid.
