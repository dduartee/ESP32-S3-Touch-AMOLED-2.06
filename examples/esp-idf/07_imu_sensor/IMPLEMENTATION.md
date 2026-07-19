# IMPLEMENTATION: IMU Sensor Dashboard (`07_imu_sensor`)

## What Was Implemented

Real-time IMU sensor dashboard reading accelerometer and gyroscope data from the QMI8658 6-axis IMU and displaying it on the AMOLED touch display via LVGL v9.

### Files Modified

| File | Change |
|---|---|
| `main/main.c` | Complete rewrite: BSP init, QMI8658 init, LVGL UI, IMU update task |
| `main/idf_component.yml` | Added `waveshare/qmi8658` dependency, added `idf: ">=5.5.0"` |
| `main/CMakeLists.txt` | Removed LVGL demo source globbing, removed audio player linking, added `PRIV_REQUIRES driver nvs_flash esp_driver_i2s esp_driver_gpio` |
| `sdkconfig.defaults` | Removed all `CONFIG_LV_USE_DEMO_*` and `CONFIG_LV_DEMO_*` lines |

### Key Design Decisions

1. **Task pinned to Core 1** — `app_main` runs on Core 0 (default), IMU task on Core 1 for isolation. Same pattern as `04_Immersive_block`.

2. **20Hz update rate** — IMU reads at 50ms intervals (`IMU_READ_DELAY_MS`), sufficient for smooth display without excessive CPU usage.

3. **Low-pass filtered angles** — Alpha = 0.1 filter on roll/pitch reduces jitter from sensor noise. Raw values are used for display labels; filtered values for the angle display.

4. **No calibration** — Skipped to keep code minimal. The `04_Immersive_block` example demonstrates calibration if needed.

5. **Chart type** — `LV_CHART_TYPE_LINE` with `LV_CHART_UPDATE_MODE_SHIFT` for a scrolling magnitude plot. Range -20 to 20 m/s² covers typical handheld motion.

6. **Thread safety** — All LVGL calls wrapped in `bsp_display_lock()`/`bsp_display_unlock()`. Sensor reads happen outside the lock.

7. **Display units** — Accelerometer in m/s² (`qmi8658_set_accel_unit_mps2`), gyroscope in rad/s (`qmi8658_set_gyro_unit_rads`), angles in degrees.

### UI Layout

- **Title**: "IMU Sensor Dashboard" (Montserrat 22, white)
- **Accelerometer section**: XYZ labels with color coding (X=red, Y=green, Z=blue)
- **Gyroscope section**: XYZ labels with same color coding
- **Angles section**: Roll and Pitch (Montserrat 20, white)
- **Chart**: 380×170 line chart with 60 data points, dark background

### How to Build and Test

```bash
cd examples/esp-idf/07_imu_sensor
idf.py build
idf.py flash monitor
```

### Test Procedure

1. Build and flash to ESP32-S3-Touch-AMOLED-2.06
2. Verify title "IMU Sensor Dashboard" appears on screen
3. Tilt the board — accelerometer XYZ values should change
4. Rotate the board — gyroscope XYZ values should show angular velocity
5. Roll/Pitch angles should update smoothly (filtered)
6. Chart should show scrolling acceleration magnitude line
7. Z accelerometer should read ~9.81 m/s² when board is flat

### Deviations from PLAN.md

- **I2C config in sdkconfig.defaults**: Skipped. The BSP handles I2C pin configuration internally via `bsp_i2c_get_handle()`. Adding `CONFIG_I2C_MASTER_SDA_IO`/`CONFIG_I2C_MASTER_SCL_IO` could cause build errors if those Kconfig symbols aren't defined in the project's sdkconfig.
- **Chart size**: 380×170 instead of 200×180 to better fill the 410px wide display.
- **Label formatting**: Used `%6.2f` for values to ensure consistent alignment in labels.
