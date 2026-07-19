# PLAN: IMU Sensor Dashboard Example (`07_imu_sensor`)

## 1. Objective

Demonstrate reading real-time accelerometer and gyroscope data from the QMI8658 6-axis IMU and displaying it on the AMOLED touch display via LVGL. The UI shows:
- Live accelerometer XYZ values (m/s²)
- Live gyroscope XYZ values (rad/s)
- Computed roll/pitch angles (degrees)
- A scrolling line chart of acceleration magnitude over time

## 2. Hardware Used

| Peripheral | Chip/Interface | Purpose |
|---|---|---|
| IMU | QMI8658 (I2C 0x6B) | Accelerometer + Gyroscope |
| Display | CO5300 (QSPI) | 410×502 AMOLED output |
| Touch | CST9220 (I2C) | Input (not used in this example, kept for BSP init) |
| PMIC | AXP2101 (I2C) | Power management (managed by BSP) |

## 3. Library Choice

**Component:** `waveshare/qmi8658` v2.0.0 from ESP Component Registry

**Why:**
- Already proven in the `04_Immersive_block` example in this same project
- Clean C API: `qmi8658_init()`, `qmi8658_is_data_ready()`, `qmi8658_read_sensor_data()`
- Uses the BSP's I2C master bus handle (`i2c_master_bus_handle_t`), so no separate I2C init needed
- Supports unit conversion (m/s² for accel, rad/s for gyro) via `qmi8658_set_accel_unit_mps2()` and `qmi8658_set_gyro_unit_rads()`
- Active maintenance (v2.0.0 uploaded 4 weeks ago)

**Version:** `*` (latest, matches 04_Immersive_block pattern)

## 4. Architecture

### Data Flow
```
QMI8658 (I2C) → qmi8658_read_sensor_data() → data struct → angle computation → LVGL widgets (via bsp_display_lock)
```

### Task Structure
| Task | Core | Priority | Function |
|---|---|---|---|
| `app_main` | Core 0 (default) | 1 | Init BSP, QMI8658, create UI, spawn sensor task |
| `imu_update_task` | Core 1 | 3 | Read IMU at 20Hz, compute angles, update LVGL |

### Thread Safety
- All LVGL calls wrapped in `bsp_display_lock()` / `bsp_display_unlock()` (same pattern as `04_Immersive_block`)
- Sensor data struct (accelX/Y/Z, gyroX/Y/Z) shared via task argument pointer — no mutex needed since writer (IMU task) is single-producer and reader (LVGL UI) consumes within the lock

## 5. IMU Configuration

| Setting | Value | Rationale |
|---|---|---|
| Accel range | ±8G | Good range for handheld tilting; avoids clipping |
| Accel ODR | 500Hz | Sufficient for display updates at 20Hz |
| Gyro range | ±512°/s | Matches 04_Immersive_block config |
| Gyro ODR | 500Hz | Matches accel ODR |
| Accel units | m/s² | Human-readable |
| Gyro units | rad/s | Standard SI |
| Display precision | 2 decimal places | Readable on 410px wide display |

### Angle Computation
- **Roll** = `atan2(accelY, accelZ)` (rotation around X axis)
- **Pitch** = `atan2(-accelX, sqrt(accelY² + accelZ²))` (rotation around Y axis)
- Convert from radians to degrees with `* (180.0f / M_PI)`
- Apply low-pass filter (α = 0.1) for smoothing: `filtered = α * new + (1-α) * filtered`

### No Calibration
This example skips calibration to keep the code minimal. The 04_Immersive_block example demonstrates calibration if needed.

## 6. LVGL UI Design

### Layout (410×502 screen)

```
┌──────────────────────────────┐
│   IMU Sensor Dashboard       │ ← Title (Montserrat 22, bold)
├──────────────────────────────┤
│  ACCELEROMETER               │ ← Section header (Montserrat 18)
│  X:  0.12 m/s²              │ ← Label (Montserrat 16, red)
│  Y: -0.05 m/s²              │ ← Label (Montserrat 16, green)
│  Z:  9.81 m/s²              │ ← Label (Montserrat 16, blue)
├──────────────────────────────┤
│  GYROSCOPE                   │ ← Section header
│  X:  0.04 rad/s             │ ← Label (Montserrat 16)
│  Y: -0.10 rad/s             │ ← Label
│  Z:  0.01 rad/s             │ ← Label
├──────────────────────────────┤
│  ANGLES                      │ ← Section header
│  Roll:   0.7°               │ ← Label (Montserrat 20)
│  Pitch: -0.3°               │ ← Label (Montserrat 20)
├──────────────────────────────┤
│ ┌──────────────────────────┐ │
│ │   Line Chart (200×180)   │ │ ← Accel magnitude over time
│ │   [scrolling 60 points]  │ │
│ └──────────────────────────┘ │
└──────────────────────────────┘
```

### Widgets
- **Title**: `lv_label` with Montserrat 22 font, centered at top
- **Section headers**: `lv_label` with Montserrat 18, bold style
- **Value labels**: `lv_label` with Montserrat 16, aligned left. Each axis color-coded (X=red, Y=green, Z=blue)
- **Angle labels**: `lv_label` with Montserrat 20, larger for emphasis
- **Chart**: `lv_chart` with `LV_CHART_TYPE_LINE`, 60 data points, range -20 to 20 (m/s²), single series (magnitude), dark background

### Update Frequency
- IMU read: 20Hz (50ms interval) — matches `TASK_DELAY_MS` from 04_Immersive_block
- LVGL refresh: governed by `CONFIG_LV_DEF_REFR_PERIOD=15` (15ms), chart updates at 20Hz
- Label text updated every 50ms via `lv_label_set_text_fmt()`

## 7. Files to Modify

| File | Changes |
|---|---|
| `main/main.c` | **Complete rewrite** — remove music demo, add QMI8658 init, IMU task, LVGL UI, angle computation |
| `main/idf_component.yml` | **Add** `waveshare/qmi8658` dependency |
| `main/CMakeLists.txt` | **Add** `PRIV_REQUIRES driver` (for I2C), remove demo sources |
| `sdkconfig.defaults` | **Add** I2C-related configs, keep existing display/touch configs |
| `CMakeLists.txt` | **No change** (project name `imu_sensor` is already correct) |

## 8. sdkconfig.defaults Changes

Add these to the existing config:

```
# I2C configuration (BSP defaults, but explicit for clarity)
CONFIG_I2C_MASTER_SDA_IO=4
CONFIG_I2C_MASTER_SCL_IO=5

# Keep all existing display, SPIRAM, and LVGL configs
```

Remove (not needed for this example):
```
CONFIG_LV_USE_DEMO_MUSIC=y
CONFIG_LV_DEMO_MUSIC_AUTO_PLAY=y
CONFIG_LV_USE_DEMO_WIDGETS=y
CONFIG_LV_USE_DEMO_BENCHMARK=y
CONFIG_LV_USE_DEMO_RENDER=y
CONFIG_LV_USE_DEMO_SCROLL=y
CONFIG_LV_USE_DEMO_STRESS=y
CONFIG_LV_USE_DEMO_TRANSFORM=y
CONFIG_LV_USE_DEMO_FLEX_LAYOUT=y
CONFIG_LV_USE_DEMO_MULTILANG=y
```

## 9. Dependencies

### idf_component.yml
```yaml
dependencies:
  espressif/esp_codec_dev:
    version: "~1.5"
    public: true
  espressif/usb:
    version: "^1.4.1"
  idf: ">=5.5.0"
  waveshare/qmi8658:
    version: "*"
  waveshare/esp32_s3_touch_amoled_2_06:
    version: "*"
  lvgl/lvgl:
    version: "9.5.0"
    public: true
```

### CMakeLists.txt (main/)
```cmake
idf_component_register(
    SRCS main.c
    INCLUDE_DIRS .
    PRIV_REQUIRES driver nvs_flash esp_driver_i2s esp_driver_gpio
)
```

Key changes from template:
- Remove `LV_DEMOS_SOURCES` and `${LV_DEMO_DIR}` include — no demos used
- Remove `LV_USE_DEMO_MUSIC` compile definition
- Remove audio player linking section
- Add `PRIV_REQUIRES driver` for I2C access

## 10. Risks / Notes

1. **I2C pin mapping**: The BSP handles I2C initialization internally. The QMI8658 component receives the bus handle from `bsp_i2c_get_handle()`. No manual I2C pin config needed.

2. **LVGL thread safety**: All `lv_label_set_text_fmt()` and chart updates MUST happen inside `bsp_display_lock()`/`bsp_display_unlock()`. The IMU task reads I2C outside the lock, then acquires the lock only for LVGL updates.

3. **Stack size**: IMU task needs 8192 bytes (same as 04_Immersive_block). Angle computation uses `math.h` functions which may need extra stack.

4. **Chart performance**: 60 data points with line chart at 20Hz is lightweight for LVGL v9. If flickering occurs, increase refresh period or reduce chart point count.

5. **Temperature data**: The QMI8658 also provides temperature. Could be added as a bonus display element if space permits.

6. **Touch interaction**: Not used in this example but BSP initializes touch. Could be extended later (e.g., tap to reset chart).
