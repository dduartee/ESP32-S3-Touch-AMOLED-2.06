# 11_ble_watch — BLE Smartwatch com Light Sleep

## Objetivo

Smartwatch BLE que recebe notificações do celular via Nordic UART Service (NUS),
exibe na tela AMOLED com LVGL, e entra em light sleep para economizar bateria.

Baseado em `09_basic_watch` (display, RTC PCF85063, AXP2101) + `10_ble_nus` (BLE NUS) + PM automático.

## Arquitetura

```
┌─────────────────────────────────────────────────────────┐
│                  11_ble_watch                            │
├─────────────┬──────────────┬──────────────┬─────────────┤
│  BSP Init   │   RTC Task   │  BLE Task    │ PM/Sleep    │
│  (display,  │  (PCF85063   │  (bt_nus:    │ (light sleep │
│   I2C, LVGL)│   read 1s)   │   NUS peri)  │  mgmt task) │
├─────────────┴──────────────┴──────────────┴─────────────┤
│  Compartilhado: NVS (boot count, NTP sync, notifs)      │
│  Periféricos: SH8601 (SPI), PCF85063 (I2C), AXP2101    │
│               GPIO0 (BOOT), BT (NimBLE)                 │
└─────────────────────────────────────────────────────────┘
```

## Comportamento

| Estado | Display | LVGL | CPU | Consumo |
|--------|---------|------|-----|---------|
| Awake | On (backlight + DCS Sleep Out) | Rodando | 240MHz | ~200mA |
| Idle → 10s | Off (backlight off + Sleep In) | Pausado | Light sleep | ~1mA |
| BT notificação chega | On | Resume | Wake | — |
| BOOT pressionado | On | Resume | Wake | — |

## Wake sources (light sleep)

| Fonte | Config | Uso |
|-------|--------|-----|
| BT (NimBLE) | Automático (controller ativo) | Notificação do celular |
| GPIO0 (BOOT) | `esp_sleep_enable_gpio_wakeup()` | Acionamento manual |
| Timer (opcional) | `esp_sleep_enable_timer_wakeup()` | Heartbeat periódico |

## Display management

- Task principal mantém contador `display_idle_ms`
- Reseta contador a cada evento: NUS RX, BOOT press
- Quando `display_idle_ms >= DISPLAY_IDLE_TIMEOUT_MS` (10s):
  1. `lv_timer_pause(lv_anim_get_timer())` — pausa animações LVGL
  2. `bsp_display_backlight_off()`
  3. Envia DCS Sleep In (0x10) via SPI
  4. Task vai dormir: `esp_light_sleep_start()`
- Ao acordar (BT ou GPIO):
  1. `bsp_display_backlight_on()`
  2. Envia DCS Sleep Out (0x11)
  3. `lv_timer_resume(lv_anim_get_timer())`
  4. Atualiza display com hora + notificação

## NUS RX: comandos do celular

| Mensagem RX | Ação |
|-------------|------|
| Texto livre | Exibe como notificação na tela |
| `>cmd:wakeup\n` | Acorda display |
| `>cmd:time:2026-07-17T14:25:00\n` | Ajusta RTC |
| `>cmd:ping\n` | Responde `>pong\n` |

## NUS TX: eventos → celular

| Evento | Mensagem TX |
|--------|-------------|
| Conectou | `>evt:connected\n` |
| Desconectou | `>evt:disconnected\n` |
| BOOT pressionado | `>cmd:btn_press\n` |
| Bateria baixa (<20%) | `>evt:battery_low:17%\n` |

## UI (LVGL)

```
┌──────────────────────────────┐
│       14:25:37               │  ← RTC, font 48
│   Wed 16/07/2026             │  ← RTC, font 24
│   ● Connected  ██ 85%        │  ← BLE status + bateria
│                              │
│   ┌──────────────────────┐   │
│   │ João: Chegando!      │   │  ← notificação, font 20
│   │ 14:25:30             │   │  ← timestamp da notif
│   └──────────────────────┘   │
│                              │
│   [Pressione BOOT]          │  ← dica
└──────────────────────────────┘
```

## Estrutura de arquivos

```
examples/esp-idf/11_ble_watch/
├── CMakeLists.txt               # project(ble_watch)
├── partitions.csv               # igual 09_basic_watch
├── sdkconfig.defaults           # 09_basic_watch + NimBLE + PM
├── main/
│   ├── main.cpp                 # lógica principal
│   ├── pcf85063.c               # RTC driver (copiado)
│   ├── pcf85063.h               # RTC driver (copiado)
│   ├── CMakeLists.txt           # SRCS: main.cpp pcf85063.c
│   └── idf_component.yml        # deps: {}
├── components/
│   ├── bt_nus/                  # copiado de 10_ble_nus
│   └── XPowersLib/              # copiado de 09_basic_watch
└── managed_components/          # lvgl, bsp (via idf_component.yml)
```

## sdkconfig.defaults — adições

Além do que `09_basic_watch` já tem:

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
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
```

## Fluxo app_main (main.cpp)

```
app_main:
  ├── log reset reason + wakeup cause
  ├── setenv TZ (BRT3)
  ├── nvs_flash_init()
  ├── bsp_display_start()          ← display + LVGL + BSP I2C
  ├── i2c_init_pmu()               ← AXP2101 no bus BSP
  ├── pcf85063_init()              ← RTC no bus BSP
  ├── create_ui()                  ← labels LVGL
  ├── axp2101_log_status()         ← bateria
  ├── bt_nus_init()                ← NimBLE NUS advertising
  │
  ├── Task: display_manager_task():
  │     loop:
  │       if display_idle_ms >= TIMEOUT:
  │         display_off()          ← backlight off + DCS Sleep In
  │         lv_timer_pause()
  │         while display_off():
  │           esp_light_sleep_start()
  │           └── wake: BT / GPIO / timer
  │           if wake_reason is NOTIFY or BOOT:
  │             display_on()       ← DCS Sleep Out + backlight on
  │             lv_timer_resume()
  │             reset_idle_timer()
  │       vTaskDelay(100ms)
  │
  ├── Task: rtc_update_task():
  │     loop a cada 1s:
  │       update_rtc_display()     ← LVGL label
  │
  └── Task: ble_status_task():
        loop a cada 2s:
          if connected: send heartbeat
          update BLE status label
```

## Dependências

- `idf_component.yml`: `lvgl/lvgl^9.2`, `espressif/esp-bsp`
- `components/XPowersLib/`: local (do 09_basic_watch)
- `components/bt_nus/`: local (do 10_ble_nus)

## Limitações da primeira versão

- Apenas última notificação visível (sem histórico)
- Comandos do celular via NUS RX (texto livre ou `>cmd:` prefixados)
- WiFi NTP mantido para sincronia RTC (re-sync a cada 7 dias)
- Sem vibra/motor — só feedback visual
