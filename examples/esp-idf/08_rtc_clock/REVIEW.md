# REVIEW: 08_rtc_clock — PCF85063 RTC Clock with LVGL Display

## 1. Summary

The ESP-IDF RTC clock example is well-structured and closely follows PLAN.md. The driver is clean, correctly uses the `i2c_master_*` API, and the LVGL UI matches the spec. However, there is a **critical I2C correctness bug** in the `pcf85063_write_reg()` function that sends the register address and data as two separate I2C transactions instead of a single combined write — this violates the I2C register-write protocol and will likely cause the device to NACK or write to the wrong register.

## 2. Plan Compliance

| PLAN.md Item | Status | Notes |
|---|---|---|
| `main/pcf85063.h` created | ✅ | 33 lines (plan: ~60) — appropriately minimal |
| `main/pcf85063.c` created | ✅ | 137 lines (plan: ~120) — slightly over |
| `main/main.c` rewritten | ✅ | 103 lines — clean, covers all required functionality |
| `main/CMakeLists.txt` updated | ✅ | `pcf85063.c` added, LV demo source removed |
| `sdkconfig.defaults` cleaned | ✅ | Demo configs removed, `MONTSERRAT_48` added |
| `main/idf_component.yml` unchanged | ✅ | Matches plan |
| I2C address 0x51 | ✅ | Matches plan |
| RAM R/W test (0x03 bit7) | ✅ | Correctly distinguishes PCF85063 from PCF8563 |
| STOP bit cleared | ✅ | `ctrl1 &= ~PCF85063_CTRL1_STOP` |
| 24-hour mode set | ✅ | `ctrl1 &= ~PCF85063_CTRL1_12H` |
| Compile-time date/time init | ✅ | `2026-07-17 12:00:00` as specified |
| LVGL fonts (48/24/16) | ✅ | All three sizes used |
| 1 Hz RTC polling via esp_timer | ✅ | 1,000,000 us period |
| LVGL thread safety (display lock) | ⚠️ | Timer callback uses lock correctly, but `create_ui()` lacks it |
| No alarm feature (per plan) | ✅ | Correctly omitted |
| Zeller's congruence for weekday | ✅ | Matches Arduino `getDayOfWeek()` results |

## 3. Register Verification

All register addresses match `PCF85063Constants.h`:

| Register | Constants.h | pcf85063.c | Match |
|---|---|---|---|
| CTRL1 | 0x00 | `PCF85063_CTRL1_REG` = 0x00 | ✅ |
| RAM | 0x03 | `PCF85063_RAM_REG` = 0x03 | ✅ |
| SEC | 0x04 | `PCF85063_SEC_REG` = 0x04 | ✅ |
| YEAR | 0x0A | `PCF85063_YEAR_REG` = 0x0A | ✅ |
| I2C Address | 0x51 | `0x51` | ✅ |

Bit masks also match:
- CTRL1 STOP: `(1 << 5)` — matches `PCF85063_CTRL1_CLOCK_EN_MASK` in Constants.h
- CTRL1 12H: `(1 << 1)` — matches `PCF85063_CTRL1_HOUR_FORMAT_12H_MASK`
- Seconds mask: `& 0x7F` (bit7 = OSC invalid) — matches Arduino `BCD2DEC(buffer[0] & 0x7F)`
- Minute mask: `& 0x7F` — matches Arduino
- Hour mask: `& 0x3F` — matches Arduino 24h mode
- Day mask: `& 0x3F` — matches Arduino
- Month mask: `& 0x1F` — matches Arduino
- Weekday mask: `& 0x07` — matches Arduino

BCD encoding/decoding is correct:
- `dec2bcd`: `(dec/10 << 4) | (dec%10)` — standard BCD
- `bcd2dec`: `(bcd>>4)*10 + (bcd&0x0F)` — standard reverse BCD

## 4. Arduino Comparison

The ESP-IDF driver covers the core RTC functionality equivalent to the Arduino SensorPCF85063 driver:

| Feature | Arduino | ESP-IDF | Match |
|---|---|---|---|
| Init probe (RAM R/W test) | ✅ `initImpl()` | ✅ `pcf85063_init()` | ✅ |
| 24h mode | ✅ `clrRegisterBit` | ✅ `ctrl1 &= ~12H` | ✅ |
| Start clock (clear STOP) | ✅ `start()` | ✅ `ctrl1 &= ~STOP` | ✅ |
| Get time | ✅ `getDateTime()` | ✅ `pcf85063_get_time()` | ✅ |
| Set time | ✅ `setDateTime()` | ✅ `pcf85063_set_time()` | ✅ |
| Day-of-week calc | ✅ `getDayOfWeek()` | ✅ `calc_weekday()` | ✅ |
| Alarm support | ✅ Full (set/get/enable) | ❌ Not implemented | N/A (by design) |
| Clock output config | ✅ `setClockOutput()` | ❌ Not implemented | N/A (by design) |
| Clock integrity check | ✅ `isClockIntegrityGuaranteed()` | ❌ Not implemented | N/A (by design) |

The BCD encode/decode buffers, register order (SEC→MIN→HOUR→DAY→WEEKDAY→MONTH→YEAR), and bit masking are all identical between implementations.

## 5. Issues Found

### Critical: I2C write splits register + data into two transactions
**`main/pcf85063.c:25-28`**

```c
static esp_err_t pcf85063_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
    return i2c_master_transmit(dev, &reg, 1, I2C_TIMEOUT_MS)
           | i2c_master_transmit(dev, &val, 1, I2C_TIMEOUT_MS);
}
```

This sends the register address as one I2C transaction, then the data as a second transaction with a repeated START/STOP between them. The PCF85063 I2C protocol requires the register address and data to be sent in a **single contiguous write** (START → ADDR+W → REG → DATA → STOP). The Arduino driver does this correctly with `writeRegister(PCF85063_SEC_REG, buffer, 7)`.

**Fix:**
```c
static esp_err_t pcf85063_write_reg(i2c_master_dev_handle_t dev, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(dev, buf, 2, I2C_TIMEOUT_MS);
}
```

Note: The only caller that matters is `pcf85063_init()` (lines 52, 62, 65, 72, 75), where this bug will prevent the RAM R/W test and STOP/12H configuration from working. The time-set function (`pcf85063_set_time`) uses a correctly formed combined payload, so time setting works.

### Medium: `create_ui()` called without display lock
**`main/main.c:90-92`**

```c
bsp_display_lock(0);
create_ui();
bsp_display_unlock();
```

Actually, this IS locked — my mistake on initial read. The lock is present. ✅ No issue.

### Medium: Device handle created/destroyed per transaction
**`main/pcf85063.c:39-80, 84-112, 114-137`**

Every call to `pcf85063_init()`, `pcf85063_set_time()`, and `pcf85063_get_time()` creates a new `i2c_master_dev_handle_t` via `i2c_master_bus_add_device()` and destroys it via `i2c_master_bus_rm_device()`. This is correct for bus sharing but adds ~100us overhead per transaction. For a 1Hz poll this is negligible, but the pattern is unusual.

**Optional improvement:** Store the device handle in `pcf85063_config_t` and create it once during init. This would require changing the config struct but would be cleaner for an example that polls frequently.

### Low: Date format deviation from PLAN.md
**`main/main.c:31`**

```c
snprintf(buf, sizeof(buf), "%s %d %d", weekday_names[t->weekday], t->month, t->day);
```

PLAN.md specifies `Thu Jul 17` format. Implementation shows `Thu 7 17` (month number, no leading zero). The IMPLEMENTATION.md documents this as intentional, which is fine for an example, but deviates from the plan.

### Low: I2C speed hardcoded to 400 kHz
**`main/pcf85063.c:43`**

```c
.scl_speed_hz = 400000,
```

The plan says "Default from BSP (100 kHz or 400 kHz)" and "I2C Speed: Default from BSP." The driver hardcodes 400 kHz. The PCF85063 supports up to 1000 kHz, so this is correct, but could conflict if the BSP sets a different bus speed. The Arduino Wire library uses whatever speed was configured externally.

### Low: `weekday_names` array not const-qualified
**`main/main.c:22-24`**

The string array could be `static const char *const weekday_names[]` for clarity, though the current form is fine.

## 6. Recommendations

1. **Fix the critical I2C write bug immediately** — this will prevent the RTC from initializing properly. The STOP bit clear and 12H mode set will fail silently or NACK.

2. **Consider using `__DATE__` and `__TIME__` macros** — PLAN.md section 9 mentions these as a risk mitigation for "Compile-time time init." Using them would make the example more practical (auto-sets to build time).

3. **Add a comment explaining the device handle create/destroy pattern** — this is non-obvious and deserves documentation for learners.

4. **Match the date format to PLAN.md** — use `%s %s %02d` with a month name lookup for `Thu Jul 17`.

## 7. Verdict

**NEEDS_REWORK**

The critical I2C write bug (`pcf85063.c:25-28`) must be fixed before this example can work on hardware. The register address and data must be sent in a single I2C transaction. All other aspects of the implementation are correct and well-structured.

**Blocking issues:**
- [ ] Fix `pcf85063_write_reg()` to use single combined write

**Non-blocking (optional):**
- [ ] Add display lock documentation
- [ ] Match date format to plan
- [ ] Consider `__DATE__`/`__TIME__` macros
- [ ] Consider storing device handle in config struct
