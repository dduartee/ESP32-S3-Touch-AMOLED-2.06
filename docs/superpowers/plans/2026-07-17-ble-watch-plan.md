# 11_ble_watch Implementation Plan

## Global Constraints

- Project: `examples/esp-idf/11_ble_watch`
- Board: ESP32-S3, AMOLED SH8601, AXP2101, PCF85063, GPIO0 BOOT
- ESP-IDF v5.5.2
- Language: C++ (`main.cpp`), `extern "C"` for `app_main`
- XPowersLib requires `.tpp` templates, `extern "C"` not needed for app_main with XPowersLib
- LVGL 9.x via `lvgl/lvgl` managed component
- BSP via `espressif/esp-bsp` managed component
- `component/bt_nus/` from `10_ble_nus` (NimBLE NUS peripheral)
- `components/XPowersLib/` from `09_basic_watch` (already exists in copy)
- `main/pcf85063.c/h` from `09_basic_watch` (already exists in copy)
- Timezone: Brazil `setenv("TZ", "BRT3", 1)`
- NTP sync every 7 days via NVS key `ntp_sync_ts` (kept from 09_basic_watch)
- WiFi NTP only activated when sync is needed (kept from 09_basic_watch)

---

## Task 1: Project Scaffold + Config

### Description
Clean up the copied 09_basic_watch directory, rename project, add bt_nus component, update CMakeLists, configure sdkconfig for BLE NimBLE + PM.

### Files

#### 1.1 CMakeLists.txt
Change project name from `basic_watch` to `ble_watch`.

#### 1.2 Remove obsolete files
```
rm -f IMPLEMENTATION.md PLAN.md REVIEW.md sdkconfig.old
rm -rf .clangd .vscode
```

#### 1.3 partitions.csv
Replace with simpler version (no SPIFFS storage partition):
```csv
# Name,   Type, SubType, Offset,  Size, Flags
nvs,      data, nvs,     0x9000,  0x6000,
phy_init, data, phy,     0xf000,  0x1000,
factory,  app,  factory, 0x10000, 0x800000,
```

#### 1.4 main/CMakeLists.txt
Remove LVGL demo sources (no music demo needed):
```cmake
idf_component_register(
    SRCS main.cpp pcf85063.c
    INCLUDE_DIRS .)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=cpp)
```

#### 1.5 components/bt_nus/
Copy from `10_ble_nus/components/bt_nus/` (both `bt_nus.c` and `bt_nus.h`).

#### 1.6 sdkconfig.defaults
Add to end of existing file:
```kconfig
# BLE NimBLE
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_COUNT=12
CONFIG_BT_NIMBLE_MSYS_1_BLOCK_SIZE=256
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=y
CONFIG_BT_NIMBLE_GATT_SERVER=y
CONFIG_BT_NIMBLE_ATT_PREFERRED_MTU=256

# Power Management
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3

# Increase main task stack for BLE + LVGL
CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192

# Increase event loop stack
CONFIG_ESP_SYSTEM_EVENT_TASK_STACK_SIZE=4096
```

#### 1.7 main/idf_component.yml
Add managed component dependencies:
```yaml
dependencies:
  lvgl/lvgl: "^9"
  espressif/esp-bsp: "^2"
```

#### 1.8 sdkconfig
Delete the old `sdkconfig` file so it regenerates on first build (otherwise stale config may conflict).

### Acceptance Criteria
- Project name is `ble_watch` in CMakeLists.txt
- `components/bt_nus/bt_nus.c` and `bt_nus.h` exist
- `sdkconfig.defaults` contains NimBLE + PM configs
- `partitions.csv` has 3 partitions (nvs, phy_init, factory)
- `main/CMakeLists.txt` compiles `main.cpp` and `pcf85063.c`
- `main/idf_component.yml` has lvlg and esp-bsp deps
- `sdkconfig` deleted

---

## Task 2: main.cpp — BLE Watch Core Implementation

### Description
Rewrite `main/main.cpp` with BLE NUS, RTC, AXP2101, LVGL UI, light sleep management, and display on/off control.

### Files
- `main/main.cpp`

### Requirements

#### 2.1 Imports and Constants
Keep all existing includes from 09_basic_watch plus add:
```cpp
#include "bt_nus.h"
```

Constants:
- `AWAKE_DISPLAY_TIMEOUT_MS = 10000` (10s display on after event)
- `RTC_UPDATE_INTERVAL_MS = 1000`
- `HEARTBEAT_INTERVAL_MS = 2000`
- WiFi SSID/PASS/TIMEOUT from 09

#### 2.2 app_main Flow
```
1. Log reset reason
2. Log wakeup cause (for debugging — will show BT, GPIO, TIMER, etc)
3. setenv TZ
4. nvs_flash_init()
5. bsp_display_start() — display + LVGL + BSP I2C
6. i2c_init_pmu() — AXP2101 on BSP I2C bus
7. pcf85063_init() — RTC on BSP I2C bus
8. create_ui() — LVGL labels
9. axp2101_log_status() — battery info
10. bt_nus_init() — NimBLE advertising
11. Check NTP sync needed (same logic as 09_basic_watch)
12. wifi_start() if sync needed
13. Create tasks:
    - rtc_update_task() — update RTC display every 1s
    - ble_heartbeat_task() — heartbeat every 2s when connected
    - display_manager_task() — light sleep + display on/off

14. Main loop: handle NTP sync completion, light sleep when idle
```

#### 2.3 Display State Machine

States:
- `DISPLAY_ON` — backlight on, LVGL active, DCS Sleep Out
- `DISPLAY_OFF` — backlight off, LVGL timers paused, DCS Sleep In

Transitions:
- `DISPLAY_ON` → timeout 10s → `display_off()`
- `DISPLAY_OFF` → NUS RX or BOOT → `display_on()`
- `DISPLAY_OFF` → no event → `esp_light_sleep_start()`

Functions:
```cpp
static void display_off(void) {
    bsp_display_lock(0);
    lv_obj_clean(lv_screen_active());
    bsp_display_unlock();
    bsp_display_backlight_off();
    // DCS Sleep In is implicit via SH8601 driver or direct SPI
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void display_on(void) {
    bsp_display_backlight_on();
    // Re-create UI labels after clean
    bsp_display_lock(0);
    create_ui();
    update_rtc_display();
    bsp_display_unlock();
}
```

#### 2.4 Light Sleep Management

`display_manager_task()` loop:
```c
void display_manager_task(void *pv) {
    while (1) {
        if (display_idle_ms >= AWAKE_DISPLAY_TIMEOUT_MS && display_state == DISPLAY_ON) {
            display_off();
            display_state = DISPLAY_OFF;
        }

        if (display_state == DISPLAY_OFF) {
            // Configure wakeup sources
            gpio_wakeup_enable(GPIO_NUM_0, GPIO_INTR_LOW_LEVEL);
            esp_sleep_enable_gpio_wakeup();
            esp_sleep_enable_timer_wakeup(60000000); // 60s periodic wake to check

            esp_light_sleep_start();
            // Woke up here — check cause
            esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
            ESP_LOGI(TAG, "Woke from light sleep: cause=%d", cause);

            if (cause == ESP_SLEEP_WAKEUP_GPIO || bt_nus_is_connected()) {
                display_on();
                display_state = DISPLAY_ON;
                display_idle_ms = 0;
            }
        }

        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
```

#### 2.5 NUS RX Notification Handling

When NUS RX data arrives, it should:
1. Log the message
2. Wake display if off
3. Show message on LVGL notification label
4. Reset idle timer
5. Forward unknown: store last notification

Modify `bt_nus.c`'s gatt handler to call a callback, OR add a global notification buffer in main.cpp that bt_nus writes to.

**Design choice:** Keep bt_nus.c unchanged (no callbacks). Instead, have main.cpp poll a "last message" buffer via a new function:
```c
// In bt_nus.h, add:
const char* bt_nus_last_rx(void);

// bt_nus.c stores last RX in a static buffer
static char last_rx[256];
// Updated in bt_nus_gatt_handler on each write
```

Then main.cpp checks `bt_nus_last_rx()` in its loop.

#### 2.6 UI Labels

```cpp
static lv_obj_t *lbl_time;       // RTC time (font 48)
static lv_obj_t *lbl_date;       // RTC date (font 24)
static lv_obj_t *lbl_status;     // BLE + battery status (font 16)
static lv_obj_t *lbl_notification; // last notification (font 20, multi-line)
```

Layout:
- `lbl_time` at center, y=-80
- `lbl_date` at center, y=-20
- `lbl_status` at center, y=30
- `lbl_notification` at center, y=70

#### 2.7 BLE Heartbeat Task
```cpp
void ble_heartbeat_task(void *pv) {
    uint32_t count = 0;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(HEARTBEAT_INTERVAL_MS));
        count++;
        if (bt_nus_is_connected()) {
            char buf[64];
            snprintf(buf, sizeof(buf), ">NUS alive count=%lu\n", (unsigned long)count);
            bt_nus_send((uint8_t*)buf, strlen(buf));
        }
    }
}
```

#### 2.8 BOOT Button Task (from 10_ble_nus)
Same debounced button logic:
- Press: send `>cmd:btn_press\n` over NUS, wake display
- Debounce: 3 consecutive same readings at 50ms interval

#### 2.9 RTC Update Task
```cpp
void rtc_update_task(void *pv) {
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(RTC_UPDATE_INTERVAL_MS));
        bsp_display_lock(0);
        update_rtc_display();
        update_status_display(); // BLE + battery
        bsp_display_unlock();
    }
}
```

#### 2.10 NTP Sync (from 09_basic_watch)
Keep the same logic:
- Check if RTC time valid and if 7-day re-sync needed
- If needed, start WiFi non-blocking
- When WiFi connects, sync NTP, set RTC, stop WiFi

#### 2.11 NVS Boot Counter (from 09_basic_watch)
Keep boot counter for debugging.

### Task 2 Acceptance Criteria
- BLE NUS advertising, connectable from nRF Connect
- Heartbeat every 2s via TX Notify
- NUS RX messages displayed on LVGL screen
- BOOT button sends `>cmd:btn_press\n` and wakes display
- Display off after 10s idle
- Light sleep entered when display off
- Wake from light sleep on BOOT or BLE notification
- RTC time displayed and updated every 1s
- AXP2101 battery status shown
- WiFi NTP sync works (kept from 09_basic_watch)
- Compiles without errors
