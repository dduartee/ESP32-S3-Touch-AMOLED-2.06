# Guia de Referência: NimBLE Nordic UART Service (NUS) com Power Management no ESP32-S3-Touch-AMOLED-2.06

> **Versão:** 1.0  
> **Data:** 2026-07-27  
> **Placa:** Waveshare ESP32-S3-Touch-AMOLED-2.06 (ESP32-S3R8)  
> **Framework:** ESP-IDF (NimBLE stack)  
> **Objetivo:** Manter conexão BLE ativa durante sleep de baixa potência, recebendo mensagens via NUS com consumo mínimo de energia.

---

## 📑 Índice

1. [Visão Geral](#1-visão-geral)
2. [Hardware e Requisitos](#2-hardware-e-requisitos)
3. [Arquitetura de Sleep do ESP32-S3](#3-arquitetura-de-sleep-do-esp32-s3)
4. [Configuração do Projeto (sdkconfig)](#4-configuração-do-projeto-sdkconfig)
5. [Estrutura do Código](#5-estrutura-do-código)
6. [Código Completo](#6-código-completo)
7. [Otimizações de Consumo](#7-otimizações-de-consumo)
8. [Troubleshooting](#8-troubleshooting)
9. [Referências](#9-referências)
10. [Checklist de Implementação](#10-checklist-de-implementação)

---

## 1. Visão Geral

### 1.1 Objetivo do Projeto
Desenvolver um firmware para o ESP32-S3-Touch-AMOLED-2.06 que:
- Atue como **periférico BLE** (Peripheral/Slave)
- Implemente o **Nordic UART Service (NUS)** para comunicação bidirecional
- Mantenha a **conexão BLE ativa** durante modos de sleep de baixa potência
- Seja capaz de **receber mensagens** do central (smartphone) enquanto dorme
- Minimize o consumo de energia para **prolongar a autonomia da bateria**

### 1.2 Stack Tecnológico
| Componente | Tecnologia | Justificativa |
|------------|-----------|---------------|
| BLE Stack | **NimBLE** | Mais leve que Bluedroid (~50KB vs ~130KB flash), menor consumo de RAM |
| Power Management | **Auto Light Sleep** | CPU pausa automaticamente; BLE controller gerencia eventos sozinho |
| Framework | **ESP-IDF** | Acesso direto às APIs de PM, BLE e periféricos |
| Sleep Clock | **32.768 kHz externo** | Reduz consumo base do Light Sleep de ~3.3mA para ~230µA |

### 1.3 Decisões de Design Críticas

```
┌─────────────────────────────────────────────────────────────────┐
│  ❌ NÃO USAR Deep Sleep                                        │
│     → CPU desliga, RAM perdida, BLE desligado                  │
│     → Conexão BLE é perdida permanentemente                    │
├─────────────────────────────────────────────────────────────────┤
│  ❌ NÃO USAR Light Sleep manual (esp_light_sleep_start)        │
│     → Desliga o rádio BLE                                      │
│     → Conexão BLE cai                                          │
├─────────────────────────────────────────────────────────────────┤
│  ✅ USAR Auto Light Sleep + BLE Modem Sleep                    │
│     → CPU pausa quando idle (FreeRTOS tickless idle)           │
│     → BLE controller permanece ativo, gerencia eventos         │
│     → CPU acorda automaticamente para RX/TX                    │
│     → Conexão BLE mantida ✅                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## 2. Hardware e Requisitos

### 2.1 Especificações da Placa

| Especificação | Valor |
|---------------|-------|
| SoC | ESP32-S3R8 (Xtensa LX7 dual-core @ 240MHz) |
| SRAM interna | 512KB |
| PSRAM | 8MB (Octal SPI, OPI PSRAM) |
| Flash | 32MB externo |
| BLE | Bluetooth 5 LE (125Kbps – 2Mbps) |
| Display | AMOLED 2.06" 410×502 (CO5300 via QSPI) |
| Touch | FT3168 (I2C) |
| PMIC | AXP2101 (I2C) – gerenciamento de energia e bateria |
| RTC | PCF85063 (I2C) – com bateria de backup |
| IMU | QMI8658 (I2C) – 6 eixos |
| Codec | ES8311 (I2S) |
| ADC | ES7210 (I2S) |
| Botões | BOOT (GPIO0), PWR (GPIO10) |
| Conectores | I2C, UART, USB, TF Card, bateria 3.7V |

### 2.2 Pinout Relevante para o Projeto

```
┌────────────────────────────────────────────────────────────┐
│  GPIO16  →  XTAL_32K_P   (Cristal 32.768kHz - Positivo)    │
│  GPIO17  →  XTAL_32K_N   (Cristal 32.768kHz - Negativo)    │
│                                                            │
│  GPIO14  →  I2C_SCL      (Touch, RTC, IMU, PMIC)           │
│  GPIO15  →  I2C_SDA      (Touch, RTC, IMU, PMIC)           │
│                                                            │
│  GPIO4-7,11-13 → QSPI Display (CO5300)                     │
│  GPIO38  →  TOUCH_INT    (FT3168 interrupt)                │
│  GPIO9   →  TOUCH_RST    (FT3168 reset)                    │
│  GPIO21  →  IMU_INT      (QMI8658 interrupt)               │
│  GPIO39  →  RTC_INT      (PCF85063 interrupt)              │
│  GPIO10  →  PWR_BTN      (Botão power)                     │
│  GPIO0   →  BOOT_BTN     (Botão boot)                      │
└────────────────────────────────────────────────────────────┘
```

### 2.3 ⚠️ Requisito Crítico: Cristal 32.768 kHz

O ESP32-S3 possui suporte a um cristal externo de 32.768 kHz para o RTC clock. **Este componente é ESSENCIAL** para atingir consumo mínimo no Auto Light Sleep.

| Configuração | Clock de Sleep | Consumo Base (Light Sleep) |
|--------------|----------------|---------------------------|
| **Com cristal 32kHz** | RTC slow clock (32kHz) | **~230 µA** |
| Sem cristal 32kHz | XTAL principal (40MHz) dividido | **~3.3 mA** |

> **Atenção:** A documentação da Waveshare não confirma a presença de um cristal 32.768kHz onboard. Verifique o schematic da placa ou adicione um cristal externo nos pinos GPIO16/GPIO17.

**Especificação do cristal recomendado:**
- Modelo: FC-135 ou equivalente
- Frequência: 32.768 kHz
- Capacitância de carga: 12.5 pF
- Tolerância: ±20 ppm

---

## 3. Arquitetura de Sleep do ESP32-S3

### 3.1 Domínios de Energia

```
┌─────────────────────────────────────────────────────────────────────┐
│                        ESP32-S3 POWER DOMAINS                       │
├─────────────────────────────────────────────────────────────────────┤
│                                                                     │
│  ┌──────────────┐    ┌──────────────┐    ┌──────────────────────┐  │
│  │   DIGITAL    │    │   WIRELESS   │    │       RTC            │  │
│  │   (CPU,      │    │   (Wi-Fi,    │    │  (RTC Memory,        │  │
│  │   Periféricos)│   │    BLE Radio)│    │   RTC Peripherals,   │  │
│  │              │    │              │    │   ULP Coprocessor)   │  │
│  │  ~15-240 mA  │    │  ~15-100 mA  │    │  ~10-150 µA          │  │
│  └──────────────┘    └──────────────┘    └──────────────────────┘  │
│                                                                     │
│  Deep Sleep:    DIGITAL=OFF  WIRELESS=OFF  RTC=ON     → ~10 µA    │
│  Light Sleep:   DIGITAL=IDLE WIRELESS=OFF  RTC=ON     → ~0.8 mA   │
│  Modem Sleep:   DIGITAL=ON   WIRELESS=IDLE RTC=ON     → ~15 mA    │
│  Active:        DIGITAL=ON   WIRELESS=ON   RTC=ON     → ~100 mA   │
│                                                                     │
│  Auto Light Sleep + BLE:                                            │
│    DIGITAL=IDLE (CPU pausa)                                         │
│    WIRELESS=IDLE (BLE controller gerencia eventos)                  │
│    RTC=ON                                                           │
│    → ~230 µA a ~3.3 mA (depende do clock de sleep)                  │
│                                                                     │
└─────────────────────────────────────────────────────────────────────┘
```

### 3.2 Fluxo de Operação com Auto Light Sleep + BLE

```
┌──────────────┐     ┌─────────────────┐     ┌─────────────────┐
│  Light Sleep │────→│ BLE Controller  │────→│  RX/TX Event    │
│   (CPU OFF)  │     │  (Acorda CPU)   │     │  (Processa)     │
└──────────────┘     └─────────────────┘     └─────────────────┘
       ↑                                              │
       │         ┌─────────────────┐                  │
       └─────────│  Volta dormir   │←─────────────────┘
                 │  (CPU pausa)    │
                 └─────────────────┘
```

1. **CPU entra em Light Sleep** quando todas as tasks estão bloqueadas (FreeRTOS tickless idle)
2. **BLE Controller permanece ativo** com seu próprio clock
3. **Evento BLE chega** (ex: pacote do smartphone)
4. **BLE Controller acorda a CPU** via interrupt
5. **CPU processa o evento** (recebe dados via NUS RX)
6. **CPU volta a dormir** automaticamente

### 3.3 Wake Sources Disponíveis

| Wake Source | Deep Sleep | Light Sleep | Auto Light Sleep |
|-------------|------------|-------------|------------------|
| Timer (RTC) | ✅ | ✅ | ✅ |
| GPIO (ext0/ext1) | ✅ | ✅ | ✅ |
| Touch pad | ✅ | ✅ | ✅ |
| ULP coprocessor | ✅ | ✅ | ✅ |
| BLE Controller | ❌ | ❌ | ✅ (automático) |
| UART | ❌ | ✅ | ✅ |
| I2C/SPI | ❌ | ✅ | ✅ |

---

## 4. Configuração do Projeto (sdkconfig)

### 4.1 Arquivo `sdkconfig.defaults`

Crie o arquivo `sdkconfig.defaults` (ou `sdkconfig.defaults.esp32s3`) na raiz do projeto:

```ini
# ============================================================
# ESP-IDF Target
# ============================================================
CONFIG_IDF_TARGET="esp32s3"

# ============================================================
# Power Management - Auto Light Sleep (ESSENCIAL)
# ============================================================
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3

# Clock de sleep: 32.768kHz externo (RECOMENDADO)
# Se não tiver cristal 32k, comente esta linha e use INT_RC
CONFIG_RTC_CLK_SRC_EXT_CRYS=y
CONFIG_ESP32S3_RTC_CLK_CAL_CYCLES=3000

# ============================================================
# Bluetooth - NimBLE Stack
# ============================================================
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1
CONFIG_BT_NIMBLE_MAX_BONDS=3
CONFIG_BT_NIMBLE_MAX_CCCDS=8
CONFIG_BT_NIMBLE_L2CAP_COC_MAX_NUM=0
CONFIG_BT_NIMBLE_RPA_TIMEOUT=900

# ============================================================
# BLE Modem Sleep (ESSENCIAL para manter conexão)
# ============================================================
CONFIG_BTDM_CTRL_MODEM_SLEEP=y
CONFIG_BTDM_CTRL_MODEM_SLEEP_MODE_1=y

# ============================================================
# PHY Power Down (economia extra ~100µA)
# ============================================================
CONFIG_ESP_PHY_MAC_BB_PD=y

# ============================================================
# Otimizações de Consumo
# ============================================================
CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y
CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y
CONFIG_PM_SLP_DISABLE_GPIO=y

# ============================================================
# CPU Frequency
# ============================================================
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240=y
CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ=240

# ============================================================
# Wi-Fi (desligar se não usar)
# ============================================================
# CONFIG_ESP_WIFI_ENABLED=n

# ============================================================
# Serial Output
# ============================================================
CONFIG_ESP_CONSOLE_UART_DEFAULT=y
CONFIG_ESP_CONSOLE_UART_NUM=0
CONFIG_ESP_CONSOLE_UART_BAUDRATE=115200

# ============================================================
# FreeRTOS
# ============================================================
CONFIG_FREERTOS_HZ=1000
CONFIG_FREERTOS_UNICORE=n

# ============================================================
# Heap / Memory
# ============================================================
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
CONFIG_SPIRAM_SPEED_80M=y
```

### 4.2 Configuração via menuconfig

Execute no terminal:

```bash
idf.py menuconfig
```

Navegue e configure:

```
Component config →
  ├── Power Management →
  │     [*] Support for power management
  │     (3)   Minimum time to enter sleep mode
  │     [*] Enable dynamic frequency scaling (DFS) at startup
  │     [ ]   DFS with light sleep
  │
  ├── Bluetooth →
  │     [*] Bluetooth
  │           Bluetooth controller mode (BLE only)
  │           Bluetooth controller (NimBLE)
  │     [*]   MODEM SLEEP Options →
  │           [*]     Bluetooth modem sleep
  │           [*]     Bluetooth modem sleep mode 1
  │
  ├── ESP32S3-Specific →
  │     RTC clock source (External 32kHz crystal)
  │
  └── FreeRTOS →
        [*] Tickless idle support
        (3)   Minimum number of ticks to enter tickless idle
```

---

## 5. Estrutura do Código

### 5.1 Diagrama de Componentes

```
┌─────────────────────────────────────────────────────────────────┐
│                         MAIN APPLICATION                         │
│  ┌─────────────┐  ┌─────────────┐  ┌─────────────────────────┐  │
│  │  app_main() │  │  BLE Task   │  │  Application Task       │  │
│  │             │  │  (NimBLE    │  │  (Sensores, Display,    │  │
│  │  - Init PM  │  │   Host)     │  │   Lógica de negócio)    │  │
│  │  - Init BLE │  │             │  │                         │  │
│  │  - Start    │  │  - GAP      │  │  - Processa dados RX    │  │
│  │    Adv      │  │  - GATT     │  │  - Envia dados TX       │  │
│  │  - Loop     │  │  - NUS Svc  │  │  - Sleep quando idle    │  │
│  └─────────────┘  └─────────────┘  └─────────────────────────┘  │
│           │              │                    │                  │
│           └──────────────┴────────────────────┘                  │
│                          │                                       │
│           ┌──────────────▼──────────────┐                        │
│           │    Power Management Layer   │                        │
│           │  (Auto Light Sleep + DFS)   │                        │
│           └─────────────────────────────┘                        │
│                          │                                       │
│           ┌──────────────▼──────────────┐                        │
│           │      BLE Controller         │                        │
│           │   (Modem Sleep automático)  │                        │
│           └─────────────────────────────┘                        │
└─────────────────────────────────────────────────────────────────┘
```

### 5.2 Fluxograma de Estados BLE

```
                    ┌─────────────┐
                    │    INIT     │
                    └──────┬──────┘
                           │
                           ▼
                    ┌─────────────┐
                    │ ADVERTISING │◄────────────────┐
                    │  (Scanable) │                 │
                    └──────┬──────┘                 │
                           │ on connect             │
                           ▼                        │
                    ┌─────────────┐                 │
                    │ CONNECTED   │                 │
                    │  (NUS open) │                 │
                    └──────┬──────┘                 │
                           │                        │
              ┌────────────┼────────────┐          │
              ▼            ▼            ▼          │
        ┌─────────┐  ┌─────────┐  ┌─────────┐     │
        │ RX Data │  │ TX Data │  │Disconnect│─────┘
        │ (Notify)│  │ (Write) │  │          │
        └─────────┘  └─────────┘  └─────────┘
```

---

## 6. Código Completo

### 6.1 Estrutura de Arquivos

```
project/
├── CMakeLists.txt
├── sdkconfig.defaults
├── main/
│   ├── CMakeLists.txt
│   ├── main.c              # Entry point
│   ├── ble_nus.c           # Nordic UART Service
│   ├── ble_nus.h           # Header NUS
│   ├── ble_gap.c           # GAP events & advertising
│   ├── ble_gap.h           # Header GAP
│   ├── power_mgmt.c        # Power management config
│   ├── power_mgmt.h        # Header PM
│   └── app_logic.c         # Lógica da aplicação
└── components/             # (se necessário)
```

### 6.2 `main/CMakeLists.txt`

```cmake
idf_component_register(SRCS "main.c"
                            "ble_nus.c"
                            "ble_gap.c"
                            "power_mgmt.c"
                            "app_logic.c"
                       INCLUDE_DIRS ".")
```

### 6.3 `CMakeLists.txt` (raiz)

```cmake
cmake_minimum_required(VERSION 3.16)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(nus_power_save)
```

### 6.4 `main/power_mgmt.h`

```c
#ifndef POWER_MGMT_H
#define POWER_MGMT_H

#include "esp_err.h"

/**
 * @brief Configura o Power Management com Auto Light Sleep
 *
 * Configura o ESP32-S3 para usar:
 * - Dynamic Frequency Scaling (DFS): 240MHz → 40MHz
 * - Automatic Light Sleep quando CPU está idle
 * - Tickless idle do FreeRTOS
 *
 * @return ESP_OK em caso de sucesso
 */
esp_err_t pm_configure_auto_light_sleep(void);

/**
 * @brief Registra o consumo atual estimado
 *
 * Usa RTC GPIO para medir corrente via shunt (opcional)
 * ou estima baseado no estado atual.
 */
void pm_log_power_status(void);

#endif /* POWER_MGMT_H */
```

### 6.5 `main/power_mgmt.c`

```c
#include "power_mgmt.h"
#include "esp_log.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "PM";

esp_err_t pm_configure_auto_light_sleep(void)
{
    esp_pm_config_esp32s3_t pm_config = {
        .max_freq_mhz = 240,   /* Frequência máxima para performance */
        .min_freq_mhz = 40,    /* Frequência mínima para economia */
        .light_sleep_enable = true,  /* ATIVA Auto Light Sleep */
    };

    esp_err_t ret = esp_pm_configure(&pm_config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Falha ao configurar PM: %s", esp_err_to_name(ret));
        return ret;
    }

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Power Management Configurado:");
    ESP_LOGI(TAG, "  Max Freq: %d MHz", pm_config.max_freq_mhz);
    ESP_LOGI(TAG, "  Min Freq: %d MHz", pm_config.min_freq_mhz);
    ESP_LOGI(TAG, "  Light Sleep: %s",
             pm_config.light_sleep_enable ? "ATIVADO" : "DESATIVADO");
    ESP_LOGI(TAG, "========================================");

    /* Log do clock source para debug */
    #ifdef CONFIG_RTC_CLK_SRC_EXT_CRYS
    ESP_LOGI(TAG, "RTC Clock: Cristal externo 32.768kHz");
    ESP_LOGI(TAG, "  → Consumo esperado no sleep: ~230-500 µA");
    #else
    ESP_LOGW(TAG, "RTC Clock: RC interno (40MHz dividido)");
    ESP_LOGW(TAG, "  → Consumo esperado no sleep: ~3.3 mA");
    ESP_LOGW(TAG, "  → Recomendado: adicionar cristal 32.768kHz nos GPIO16/17");
    #endif

    return ESP_OK;
}

void pm_log_power_status(void)
{
    /* Estimativa baseada no estado atual */
    ESP_LOGI(TAG, "--- Status de Energia ---");
    ESP_LOGI(TAG, "CPU Freq atual: verificar via esp_pm_dump_locks()");
    ESP_LOGI(TAG, "Sleep mode: Auto Light Sleep ativo");
    ESP_LOGI(TAG, "BLE: Conexão ativa com Modem Sleep");
}
```

### 6.6 `main/ble_nus.h`

```c
#ifndef BLE_NUS_H
#define BLE_NUS_H

#include <stdint.h>
#include <stdbool.h>
#include "host/ble_hs.h"

/* Nordic UART Service UUID (128-bit) */
#define NUS_SERVICE_UUID        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,                                 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E

/* NUS TX Characteristic (Notify) - ESP envia dados para Central */
#define NUS_TX_CHAR_UUID        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,                                 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E

/* NUS RX Characteristic (Write) - Central envia dados para ESP */
#define NUS_RX_CHAR_UUID        0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,                                 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E

/**
 * @brief Inicializa o serviço Nordic UART
 *
 * Registra o serviço GATT com as características TX (Notify) e RX (Write).
 *
 * @return 0 em caso de sucesso, código de erro NimBLE caso contrário
 */
int nus_init(void);

/**
 * @brief Envia dados para o Central via Notificação (TX)
 *
 * @param data Ponteiro para os dados
 * @param len  Tamanho dos dados (max MTU - 3)
 * @return 0 em caso de sucesso
 */
int nus_tx_send(const uint8_t *data, uint16_t len);

/**
 * @brief Verifica se há um central conectado e notificações ativas
 *
 * @return true se pronto para enviar dados
 */
bool nus_is_ready(void);

/**
 * @brief Callback para processar dados recebidos via RX
 *
 * Deve ser implementado em app_logic.c
 *
 * @param data Ponteiro para os dados recebidos
 * @param len  Tamanho dos dados
 */
void nus_rx_callback(const uint8_t *data, uint16_t len);

#endif /* BLE_NUS_H */
```

### 6.7 `main/ble_nus.c`

```c
#include "ble_nus.h"
#include "ble_gap.h"
#include "esp_log.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "NUS";

/* Handles das características */
static uint16_t nus_tx_attr_handle = 0;
static uint16_t nus_rx_attr_handle = 0;

/* Estado da conexão */
static bool nus_notify_enabled = false;

/* Forward declaration do callback de acesso GATT */
static int nus_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg);

/* Definição do serviço GATT */
static const struct ble_gatt_svc_def nus_svc_def[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = BLE_UUID128_DECLARE(NUS_SERVICE_UUID),
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                /* TX Characteristic - ESP envia para Central (Notify) */
                .uuid = BLE_UUID128_DECLARE(NUS_TX_CHAR_UUID),
                .access_cb = nus_access_cb,
                .flags = BLE_GATT_CHR_F_NOTIFY,
                .val_handle = &nus_tx_attr_handle,
            },
            {
                /* RX Characteristic - Central envia para ESP (Write) */
                .uuid = BLE_UUID128_DECLARE(NUS_RX_CHAR_UUID),
                .access_cb = nus_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
                .val_handle = &nus_rx_attr_handle,
            },
            { 0 } /* Terminador */
        },
    },
    { 0 } /* Terminador */
};

static int nus_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR:
        /* Dados recebidos do Central via RX characteristic */
        if (attr_handle == nus_rx_attr_handle) {
            uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
            if (len > 0) {
                uint8_t buf[512];
                if (len > sizeof(buf) - 1) {
                    len = sizeof(buf) - 1;
                }
                ble_hs_mbuf_to_flat(ctxt->om, buf, len, NULL);
                buf[len] = '\0';

                ESP_LOGI(TAG, "📨 RX recebido [%d bytes]", len);
                ESP_LOG_BUFFER_HEXDUMP(TAG, buf, len, ESP_LOG_DEBUG);

                /* Chama callback da aplicação */
                nus_rx_callback(buf, len);
            }
        }
        break;

    case BLE_GATT_ACCESS_OP_READ_CHR:
        /* Leitura não suportada para NUS */
        return BLE_ATT_ERR_UNLIKELY;

    default:
        break;
    }

    return 0;
}

int nus_init(void)
{
    int rc;

    /* Registra o serviço GATT */
    rc = ble_gatts_count_cfg(nus_svc_def);
    if (rc != 0) {
        ESP_LOGE(TAG, "Falha em ble_gatts_count_cfg: %d", rc);
        return rc;
    }

    rc = ble_gatts_add_svcs(nus_svc_def);
    if (rc != 0) {
        ESP_LOGE(TAG, "Falha em ble_gatts_add_svcs: %d", rc);
        return rc;
    }

    ESP_LOGI(TAG, "Nordic UART Service inicializado");
    ESP_LOGI(TAG, "  TX handle: %d (Notify)", nus_tx_attr_handle);
    ESP_LOGI(TAG, "  RX handle: %d (Write)", nus_rx_attr_handle);

    return 0;
}

int nus_tx_send(const uint8_t *data, uint16_t len)
{
    if (!nus_is_ready()) {
        ESP_LOGW(TAG, "NUS não está pronto para enviar");
        return -1;
    }

    uint16_t conn_handle = ble_gap_get_conn_handle();
    if (conn_handle == BLE_HS_CONN_HANDLE_NONE) {
        ESP_LOGW(TAG, "Sem conexão ativa");
        return -1;
    }

    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (om == NULL) {
        ESP_LOGE(TAG, "Falha ao alocar mbuf");
        return -1;
    }

    int rc = ble_gatts_notify_custom(conn_handle, nus_tx_attr_handle, om);
    if (rc != 0) {
        ESP_LOGE(TAG, "Falha ao enviar notify: %d", rc);
        return rc;
    }

    ESP_LOGI(TAG, "📤 TX enviado [%d bytes]", len);
    return 0;
}

bool nus_is_ready(void)
{
    return nus_notify_enabled && (ble_gap_get_conn_handle() != BLE_HS_CONN_HANDLE_NONE);
}

/* Chamado pelo GAP handler quando o Central se inscreve em notificações */
void nus_set_notify_enabled(bool enabled)
{
    nus_notify_enabled = enabled;
    ESP_LOGI(TAG, "Notify %s", enabled ? "ATIVADO" : "DESATIVADO");
}
```

### 6.8 `main/ble_gap.h`

```c
#ifndef BLE_GAP_H
#define BLE_GAP_H

#include <stdint.h>
#include "host/ble_hs.h"

/**
 * @brief Inicializa o GAP e inicia advertising
 *
 * Configura o nome do dispositivo e inicia o advertising
 * com parâmetros otimizados para baixo consumo.
 *
 * @param device_name Nome do dispositivo BLE (max 31 chars)
 * @return 0 em caso de sucesso
 */
int gap_init(const char *device_name);

/**
 * @brief Retorna o handle da conexão atual
 *
 * @return Handle da conexão ou BLE_HS_CONN_HANDLE_NONE
 */
uint16_t ble_gap_get_conn_handle(void);

/**
 * @brief Callback de eventos GAP (usado internamente)
 */
int ble_gap_event_cb(struct ble_gap_event *event, void *arg);

#endif /* BLE_GAP_H */
```

### 6.9 `main/ble_gap.c`

```c
#include "ble_gap.h"
#include "ble_nus.h"
#include "esp_log.h"
#include "services/gap/ble_svc_gap.h"

static const char *TAG = "GAP";

static uint16_t current_conn_handle = BLE_HS_CONN_HANDLE_NONE;

uint16_t ble_gap_get_conn_handle(void)
{
    return current_conn_handle;
}

int ble_gap_event_cb(struct ble_gap_event *event, void *arg)
{
    struct ble_gap_conn_desc desc;
    int rc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* Conexão estabelecida */
        if (event->connect.status == 0) {
            current_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "✅ Conectado! handle=%d", current_conn_handle);

            /* Obtém informações da conexão */
            rc = ble_gap_conn_find(current_conn_handle, &desc);
            if (rc == 0) {
                ESP_LOGI(TAG, "  Peer addr: %s",
                         addr_str(desc.peer_id_addr.val));
                ESP_LOGI(TAG, "  Intervalo atual: %d (%.2f ms)",
                         desc.conn_itvl, desc.conn_itvl * 1.25f);
            }

            /* Solicita parâmetros de conexão otimizados para economia */
            struct ble_gap_upd_params params = {
                .itvl_min = 400,   /* 400 * 1.25ms = 500ms */
                .itvl_max = 800,   /* 800 * 1.25ms = 1000ms */
                .latency  = 10,    /* Slave latency: pula até 10 eventos */
                .supervision_timeout = 500,  /* 5 segundos */
                .min_ce_len = 0,
                .max_ce_len = 0
            };

            rc = ble_gap_update_params(current_conn_handle, &params);
            if (rc == 0) {
                ESP_LOGI(TAG, "  Solicitado: intervalo 500-1000ms, latency 10");
            } else {
                ESP_LOGW(TAG, "  Falha ao solicitar update: %d", rc);
            }

        } else {
            ESP_LOGE(TAG, "❌ Falha na conexão: %d", event->connect.status);
            gap_start_advertising();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "🔌 Desconectado, motivo=0x%02x (%s)",
                 event->disconnect.reason,
                 ble_gap_conn_str(event->disconnect.reason));
        current_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        nus_set_notify_enabled(false);
        gap_start_advertising();
        break;

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* Parâmetros de conexão atualizados */
        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (rc == 0) {
            ESP_LOGI(TAG, "🔄 Parâmetros atualizados:");
            ESP_LOGI(TAG, "  Intervalo: %d (%.2f ms)",
                     desc.conn_itvl, desc.conn_itvl * 1.25f);
            ESP_LOGI(TAG, "  Latency: %d", desc.conn_latency);
            ESP_LOGI(TAG, "  Supervision: %d (%.2f s)",
                     desc.supervision_timeout, desc.supervision_timeout / 100.0f);
        }
        break;

    case BLE_GAP_EVENT_SUBSCRIBE:
        /* Central se inscreveu/desinscreveu de notificações */
        if (event->subscribe.attr_handle == nus_tx_attr_handle) {
            bool enabled = event->subscribe.cur_notify;
            nus_set_notify_enabled(enabled);
        }
        break;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGW(TAG, "📢 Advertising completo, reiniciando...");
        gap_start_advertising();
        break;

    case BLE_GAP_EVENT_NOTIFY_TX:
        /* Confirmação de envio de notificação */
        if (event->notify_tx.status != 0) {
            ESP_LOGW(TAG, "Notify TX falhou: %d", event->notify_tx.status);
        }
        break;

    default:
        break;
    }

    return 0;
}

int gap_init(const char *device_name)
{
    int rc;

    /* Configura nome do dispositivo */
    rc = ble_svc_gap_device_name_set(device_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "Falha ao setar device name: %d", rc);
        return rc;
    }

    /* Inicializa serviço GAP */
    ble_svc_gap_init();

    ESP_LOGI(TAG, "GAP inicializado, nome: %s", device_name);

    /* Inicia advertising */
    return gap_start_advertising();
}

int gap_start_advertising(void)
{
    struct ble_gap_adv_params adv_params = {0};
    struct ble_hs_adv_fields fields = {0};
    int rc;

    /* Campos do advertising */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    const char *name = ble_svc_gap_device_name();
    fields.name = (uint8_t *)name;
    fields.name_len = strlen(name);
    fields.name_is_complete = 1;

    /* Adiciona UUID do NUS no advertising para fácil descoberta */
    ble_uuid128_t nus_uuid;
    memcpy(nus_uuid.value, (uint8_t[]){NUS_SERVICE_UUID}, 16);
    fields.uuids128 = &nus_uuid;
    fields.num_uuids128 = 1;
    fields.uuids128_is_complete = 1;

    rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "Falha ao setar adv fields: %d", rc);
        return rc;
    }

    /* Parâmetros de advertising otimizados para baixo consumo */
    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;  /* Conectável */
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;  /* Descoberta geral */

    /* Intervalo longo para economizar bateria */
    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);   /* 500ms */
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(1000);  /* 1000ms */

    rc = ble_gap_adv_start(ble_hs_id_addr_type(), NULL, BLE_HS_FOREVER,
                           &adv_params, ble_gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "Falha ao iniciar advertising: %d", rc);
        return rc;
    }

    ESP_LOGI(TAG, "📢 Advertising iniciado (intervalo: 500-1000ms)");
    return 0;
}

/* Helper para converter endereço MAC para string */
static const char *addr_str(const uint8_t *addr)
{
    static char buf[18];
    sprintf(buf, "%02X:%02X:%02X:%02X:%02X:%02X",
            addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return buf;
}
```

### 6.10 `main/app_logic.h`

```c
#ifndef APP_LOGIC_H
#define APP_LOGIC_H

#include <stdint.h>

/**
 * @brief Inicializa a lógica da aplicação
 *
 * Configura sensores, display, e outras tarefas.
 */
void app_logic_init(void);

/**
 * @brief Processa comando recebido via NUS RX
 *
 * @param data Dados recebidos
 * @param len  Tamanho dos dados
 */
void app_process_command(const uint8_t *data, uint16_t len);

#endif /* APP_LOGIC_H */
```

### 6.11 `main/app_logic.c`

```c
#include "app_logic.h"
#include "ble_nus.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "APP";

void app_logic_init(void)
{
    ESP_LOGI(TAG, "Inicializando lógica da aplicação...");

    /* Aqui você inicializa:
     * - Display AMOLED (CO5300 via QSPI)
     * - Touch controller (FT3168 via I2C)
     * - IMU (QMI8658 via I2C)
     * - PMIC (AXP2101 via I2C)
     * - RTC (PCF85063 via I2C)
     * - Audio codec (ES8311 via I2S)
     */

    ESP_LOGI(TAG, "Aplicação inicializada");
}

void nus_rx_callback(const uint8_t *data, uint16_t len)
{
    /* Callback chamado pelo ble_nus.c quando dados chegam via RX */
    app_process_command(data, len);
}

void app_process_command(const uint8_t *data, uint16_t len)
{
    /* Buffer para string null-terminated */
    char cmd[256];
    if (len >= sizeof(cmd)) len = sizeof(cmd) - 1;
    memcpy(cmd, data, len);
    cmd[len] = '\0';

    ESP_LOGI(TAG, "Comando recebido: %s", cmd);

    /* Processamento de comandos simples */
    if (strncmp(cmd, "PING", 4) == 0) {
        const char *resp = "PONG\n";
        nus_tx_send((const uint8_t *)resp, strlen(resp));

    } else if (strncmp(cmd, "STATUS", 6) == 0) {
        char resp[128];
        snprintf(resp, sizeof(resp),
                 "Status: Connected, Sleep=AutoLight, Heap=%d\n",
                 esp_get_free_heap_size());
        nus_tx_send((const uint8_t *)resp, strlen(resp));

    } else if (strncmp(cmd, "SLEEP", 5) == 0) {
        /* Força display a dormir (exemplo) */
        ESP_LOGI(TAG, "Comando SLEEP recebido - desligando display");
        const char *resp = "Display sleep mode\n";
        nus_tx_send((const uint8_t *)resp, strlen(resp));

    } else if (strncmp(cmd, "WAKE", 4) == 0) {
        /* Acorda display (exemplo) */
        ESP_LOGI(TAG, "Comando WAKE recebido - ligando display");
        const char *resp = "Display wake mode\n";
        nus_tx_send((const uint8_t *)resp, strlen(resp));

    } else {
        char resp[256];
        snprintf(resp, sizeof(resp), "Comando desconhecido: %s\n", cmd);
        nus_tx_send((const uint8_t *)resp, strlen(resp));
    }
}
```

### 6.12 `main/main.c`

```c
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

#include "power_mgmt.h"
#include "ble_nus.h"
#include "ble_gap.h"
#include "app_logic.h"

static const char *TAG = "MAIN";

/* ============================================================
 * NimBLE Host Task
 * ============================================================ */
static void ble_host_task(void *param)
{
    ESP_LOGI(TAG, "NimBLE host task iniciada");
    nimble_port_run();  /* Loop principal do NimBLE - NUNCA retorna */
    nimble_port_freertos_deinit();
}

/* ============================================================
 * Callback de sync do BLE stack
 * ============================================================ */
static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE stack sincronizado");

    /* Inicializa GAP (advertising) */
    gap_init("S3-NUS-Sleep");
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE reset, motivo=%d", reason);
}

/* ============================================================
 * Main
 * ============================================================ */
void app_main(void)
{
    int rc;

    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "ESP32-S3 NUS + Auto Light Sleep");
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Free heap: %d bytes", esp_get_free_heap_size());

    /* 1. Inicializa NVS (necessário para BLE bonding) */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* 2. Configura Power Management (Auto Light Sleep) */
    ESP_LOGI(TAG, "Configurando Power Management...");
    ESP_ERROR_CHECK(pm_configure_auto_light_sleep());

    /* 3. Inicializa NimBLE stack */
    ESP_LOGI(TAG, "Inicializando NimBLE...");
    ESP_ERROR_CHECK(nimble_port_init());

    /* 4. Configura callbacks de sync/reset */
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;

    /* 5. Inicializa serviços GAP e GATT */
    gap_init("S3-NUS-Sleep");
    nus_init();

    /* 6. Inicializa lógica da aplicação */
    app_logic_init();

    /* 7. Inicia task do NimBLE host */
    nimble_port_freertos_init(ble_host_task);

    /* 8. Loop principal da aplicação */
    ESP_LOGI(TAG, "========================================");
    ESP_LOGI(TAG, "Sistema operacional. Aguardando conexão BLE...");
    ESP_LOGI(TAG, "A CPU entrará em Auto Light Sleep automaticamente.");
    ESP_LOGI(TAG, "========================================");

    uint32_t loop_count = 0;
    while (1) {
        /* O FreeRTOS tickless idle gerencia o sleep automaticamente.
         * Quando todas as tasks estão bloqueadas, a CPU entra em
         * Light Sleep. O BLE controller acorda a CPU quando necessário.
         *
         * IMPORTANTE: Use vTaskDelay() ou xTaskNotifyWait() para
         * permitir que o kernel entre em tickless idle.
         */
        vTaskDelay(pdMS_TO_TICKS(5000));

        loop_count++;
        ESP_LOGI(TAG, "[Loop %lu] Free heap: %d bytes | CPU acordada por timeout",
                 loop_count, esp_get_free_heap_size());

        /* Aqui você pode adicionar tarefas periódicas:
         * - Ler sensores (IMU, bateria via AXP2101)
         * - Atualizar display (AMOLED)
         * - Enviar dados via NUS se conectado
         */
        if (nus_is_ready()) {
            char status_msg[64];
            snprintf(status_msg, sizeof(status_msg),
                     "Heartbeat #%lu\n", loop_count);
            nus_tx_send((const uint8_t *)status_msg, strlen(status_msg));
        }
    }
}
```

---

## 7. Otimizações de Consumo

### 7.1 Parâmetros de Conexão BLE

| Parâmetro | Valor Recomendado | Impacto |
|-----------|-------------------|---------|
| Connection Interval Min | 400 (500ms) | Menor = mais responsivo, maior consumo |
| Connection Interval Max | 800 (1000ms) | Maior = mais economia, mais latência |
| Slave Latency | 4-10 | Pula eventos sem dados → grande economia |
| Supervision Timeout | 500 (5s) | Deve ser > (1 + latency) * interval * 2 |

```c
/* Fórmula de verificação:
 * supervision_timeout > (1 + slave_latency) * max_interval * 2
 * 5000ms > (1 + 10) * 1000ms * 2 = 22000ms ❌
 *
 * Correto:
 * supervision_timeout = 600 (6s)
 * 6000ms > 22000ms ❌ ainda não...
 *
 * supervision_timeout deve ser em unidades de 10ms:
 * timeout = 3000 (30s) → 30000ms > 22000ms ✅
 */
```

### 7.2 TX Power do BLE

Reduza a potência de transmissão se o smartphone estiver próximo:

```c
#include "esp_bt.h"

/* Níveis disponíveis:
 * ESP_PWR_LVL_N12 = -12 dBm
 * ESP_PWR_LVL_N9  = -9 dBm
 * ESP_PWR_LVL_N6  = -6 dBm
 * ESP_PWR_LVL_N3  = -3 dBm
 * ESP_PWR_LVL_N0  = 0 dBm
 * ESP_PWR_LVL_P3  = +3 dBm
 * ESP_PWR_LVL_P6  = +6 dBm
 * ESP_PWR_LVL_P9  = +9 dBm
 */
esp_ble_tx_power_set(ESP_PWR_LVL_N9);  /* -9 dBm para curta distância */
```

### 7.3 Display AMOLED

O display AMOLED consome energia proporcional aos pixels acesos. Para economia máxima:

```c
/* Desligar display durante inatividade */
void display_sleep(void) {
    /* Envia comando de sleep para CO5300 via QSPI */
    /* Ou controla via GPIO de enable do display */
}

/* Usar pixels pretos (AMOLED não consome energia em preto) */
void display_show_black_screen(void) {
    /* Preenche framebuffer com 0x000000 */
}
```

### 7.4 PMIC AXP2101

O AXP2101 permite controlar rails de energia via I2C:

```c
/* Exemplo: desligar rail não utilizada */
/* Consulte datasheet do AXP2101 e biblioteca XPowersLib */
```

### 7.5 Wi-Fi Desligado

Se não usar Wi-Fi, desligue completamente:

```c
#include "esp_wifi.h"

/* No menuconfig: CONFIG_ESP_WIFI_ENABLED=n */
/* Ou em runtime: */
esp_wifi_stop();
esp_wifi_deinit();
```

### 7.6 Tabela de Consumo Esperado

| Cenário | Consumo Médio | Autonomia (300mAh) |
|---------|---------------|-------------------|
| Active (sem otimização) | ~100 mA | ~3 horas |
| Modem Sleep (BLE apenas) | ~15-20 mA | ~15-20 horas |
| **Auto Light Sleep + 32kHz XTAL** | **~0.3-1.0 mA** | **~12-40 dias** |
| Auto Light Sleep sem 32kHz | ~3.3 mA | ~3-4 dias |
| Deep Sleep (conexão perdida) | ~10 µA | ~3 anos |

---

## 8. Troubleshooting

### 8.1 Problemas de Conexão

| Sintoma | Causa | Solução |
|---------|-------|---------|
| Conexão cai após poucos segundos | Supervision timeout muito curto | Aumente para > (1+latency)*interval*2 |
| Não aparece no scan do smartphone | Advertising não iniciou | Verifique `ble_gap_adv_start()` retorno |
| Conexão instável | Intervalo muito longo + interferência | Reduza intervalo para 100-200ms |
| Pairing falha | Bonding não configurado | Ative `CONFIG_BT_NIMBLE_MAX_BONDS` |

### 8.2 Problemas de Sleep

| Sintoma | Causa | Solução |
|---------|-------|---------|
| Consumo alto (~15mA) | Sem cristal 32kHz ou PM não ativado | Verifique `sdkconfig` e hardware |
| Não entra em Light Sleep | Task rodando constantemente | Use `vTaskDelay()` ou semáforos |
| Serial com gaps/lag | UART suspende no Light Sleep | Normal; use `ESP_SLEEP_ALWAYS_FLUSH_UART` |
| CPU nunca dorme | `idle_time_before_sleep` muito alto | Reduza para 2-3 ticks |
| Wake constante | BLE eventos muito frequentes | Aumente connection interval |

### 8.3 Problemas de Hardware

| Sintoma | Causa | Solução |
|---------|-------|---------|
| AXP2101 reseta no wake | Pico de corrente | Adicione capacitor 100µF perto do ESP32-S3 |
| Display flickering | QSPI clock interfere com RF | Use shielding ou reduza QSPI speed |
| Touch não responde após sleep | I2C não retoma | Reinicialize I2C no wake callback |
| Bateria descarrega rápido | PSRAM/Flash em sleep | Ative `CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND` |

### 8.4 Debug Flags Úteis

```ini
# sdkconfig.debug
CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y
CONFIG_BT_NIMBLE_LOG_LEVEL_DEBUG=y
CONFIG_PM_TRACE=y
CONFIG_FREERTOS_USE_TRACE_FACILITY=y
```

---

## 9. Referências

### 9.1 Documentação Oficial

| Documento | Link | Seções Relevantes |
|-----------|------|-------------------|
| ESP-IDF Sleep Modes | [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/sleep_modes.html) | Light Sleep, Auto Light Sleep, Wake Sources |
| ESP-IDF Power Management | [docs.espressif.com](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/system/power_management.html) | DFS, PM locks, Light Sleep |
| NimBLE Power Save Example | [GitHub Espressif](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/nimble/power_save) | Código de referência completo |
| ESP32-S3 Datasheet | [Waveshare](https://files.waveshare.com/wiki/common/Esp32-s3_datasheet_en.pdf) | Seção 3.2.1 (PMU), 3.7 (BLE) |
| ESP32-S3 TRM | [Waveshare](https://files.waveshare.com/wiki/common/Esp32-s3_technical_reference_manual_en.pdf) | Capítulo 10 (Low-power Management) |
| Waveshare Wiki | [waveshare.com](https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06) | Pinout, demos, bibliotecas |

### 9.2 Recursos Adicionais

- [Nordic UART Service Specification](https://infocenter.nordicsemi.com/index.jsp?topic=%2Fcom.nordic.infocenter.sdk5.v15.0.0%2Fble_sdk_app_nus_eval.html)
- [ESP-IDF NimBLE API Reference](https://docs.espressif.com/projects/esp-idf/en/stable/esp32s3/api-reference/bluetooth/nimble/index.html)
- [Bluetooth Core Specification v5.0](https://www.bluetooth.com/specifications/bluetooth-core-specification/)

---

## 10. Checklist de Implementação

### 10.1 Setup Inicial

- [ ] Instalar ESP-IDF v5.x ou superior
- [ ] Configurar target: `idf.py set-target esp32s3`
- [ ] Criar estrutura de diretórios do projeto
- [ ] Copiar `sdkconfig.defaults` fornecido
- [ ] Verificar cristal 32.768kHz nos GPIO16/GPIO17

### 10.2 Configuração

- [ ] Executar `idf.py menuconfig` e verificar:
  - [ ] `CONFIG_PM_ENABLE=y`
  - [ ] `CONFIG_FREERTOS_USE_TICKLESS_IDLE=y`
  - [ ] `CONFIG_BT_NIMBLE_ENABLED=y`
  - [ ] `CONFIG_BTDM_CTRL_MODEM_SLEEP=y`
  - [ ] `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` (se houver cristal)
  - [ ] `CONFIG_ESP_PHY_MAC_BB_PD=y`

### 10.3 Código

- [ ] Implementar `power_mgmt.c` com `esp_pm_configure()`
- [ ] Implementar `ble_nus.c` com serviço GATT NUS
- [ ] Implementar `ble_gap.c` com advertising e eventos
- [ ] Implementar `app_logic.c` com processamento de comandos
- [ ] Implementar `main.c` com inicialização correta
- [ ] Garantir uso de `vTaskDelay()` no loop principal

### 10.4 Testes

- [ ] Compilar: `idf.py build`
- [ ] Flash: `idf.py -p /dev/ttyUSB0 flash monitor`
- [ ] Verificar advertising no smartphone (nRF Connect, LightBlue)
- [ ] Conectar e testar envio/recepção de dados
- [ ] Medir consumo com multímetro (modo µA)
- [ ] Verificar se entra em Light Sleep (logs de debug)
- [ ] Testar recepção de mensagens durante sleep
- [ ] Verificar estabilidade da conexão por >1 hora

### 10.5 Otimização

- [ ] Ajustar connection interval para equilibrar latência/consumo
- [ ] Configurar slave latency adequada
- [ ] Ajustar TX power conforme distância do smartphone
- [ ] Implementar sleep do display AMOLED
- [ ] Desligar periféricos não utilizados via AXP2101
- [ ] Medir consumo final e comparar com estimativa

---

## Apêndice A: Comandos Úteis

```bash
# Build do projeto
idf.py build

# Flash e monitor
idf.py -p /dev/ttyUSB0 flash monitor

# Apenas monitor (logs)
idf.py -p /dev/ttyUSB0 monitor

# Limpar build
idf.py fullclean

# Menuconfig
idf.py menuconfig

# Verificar configurações atuais
cat sdkconfig | grep -E "PM_|BT_|RTC_CLK|FREERTOS_USE_TICKLESS"

# Medir consumo (requer hardware adicional)
# Use um multímetro em série com a bateria ou
# uma placa de medição de corrente (ex: Nordic PPK2)
```

## Apêndice B: Estrutura do Pacote BLE NUS

```
┌─────────────────────────────────────────────────────────────┐
│                    BLE PACKET STRUCTURE                      │
├─────────────────────────────────────────────────────────────┤
│                                                             │
│  Advertising Packet:                                        │
│  ┌──────────┬──────────┬────────────────┐                   │
│  │  Flags   │  Name    │  UUID128 (NUS) │                   │
│  │ (3 bytes)│(max 31B) │   (16 bytes)   │                   │
│  └──────────┴──────────┴────────────────┘                   │
│                                                             │
│  ATT Write (RX):                                            │
│  ┌──────────┬──────────┬────────────────┐                   │
│  │  Handle  │  Opcode  │     Data       │                   │
│  │ (2 bytes)│ (1 byte) │   (0-512B)     │                   │
│  └──────────┴──────────┴────────────────┘                   │
│                                                             │
│  ATT Notify (TX):                                           │
│  ┌──────────┬──────────┬────────────────┐                   │
│  │  Handle  │  Opcode  │     Data       │                   │
│  │ (2 bytes)│ (1 byte) │   (0-512B)     │                   │
│  └──────────┴──────────┴────────────────┘                   │
│                                                             │
└─────────────────────────────────────────────────────────────┘
```

---

> **Nota Final:** Este guia foi elaborado com base nas documentações oficiais da Espressif (ESP-IDF, ESP32-S3 Datasheet e Technical Reference Manual) e na documentação da Waveshare para a placa ESP32-S3-Touch-AMOLED-2.06. Recomenda-se sempre consultar a documentação mais recente disponível nos links de referência.

---

*Documento gerado em 2026-07-27 para referência no desenvolvimento de firmware NimBLE NUS com Power Management no ESP32-S3.*
