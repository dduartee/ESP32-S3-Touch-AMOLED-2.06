# Referência de Desenvolvimento — BLE NUS + Light-Sleep no ESP32-S3

> Documento de referência técnica para implementação de uma conexão NimBLE (Nordic UART Service)
> no ESP32-S3 que sobrevive a light-sleep, permanece capaz de receber mensagens do central, e evita
> os erros mais comuns de gerenciamento de energia com BLE ativo.

---


## 1. Visão geral do projeto

**Objetivo:** manter uma conexão BLE ativa usando o Nordic UART Service (NUS) no ESP32-S3, atravessando
períodos de sleep, sem perder a conexão e sem perder mensagens recebidas do lado central.

**Papéis assumidos:**

- **ESP32-S3** → periférico / GATT **server** (anuncia o serviço NUS, aceita conexão).
- **Central** → dispositivo externo (telefone, PC, etc.) que conecta e escreve na característica RX.

> Se o papel real do seu projeto for o inverso (ESP32-S3 como central), o mecanismo de sleep é o mesmo,
> mas a lógica de GAP/GATT muda (scan + connect em vez de advertise + accept). Ajustar a Seção 8 conforme
> necessário.

**Resultado esperado:** o ESP32-S3 passa a maior parte do tempo em light-sleep, acorda automaticamente
a cada evento de conexão BLE agendado, processa qualquer escrita pendente na característica RX, e volta
a dormir — tudo de forma transparente, sem sleep manual programado pelo firmware.

---

## 2. Hardware de referência

Placa: **Waveshare ESP32-S3-Touch-AMOLED-2.06** (formato smartwatch).

| Item | Especificação |
|---|---|
| Chip | ESP32-S3R8 (Xtensa LX7 dual-core, até 240 MHz) |
| RAM interna | 512 KB SRAM + 16 KB SRAM na RTC |
| PSRAM | 8 MB, **Octal SPI**, embutida no chip (R8) |
| Flash | 32 MB externa |
| Rádio | Wi-Fi 802.11 b/g/n + Bluetooth 5 (LE) |
| Tela | AMOLED 2.06", 410×502, touch (FT3168, I2C) |
| PMU | AXP2101 (gerencia bateria/carregamento) |
| RTC externo | PCF85063 (I2C, alimentado via AXP2101) |
| IMU | QMI8658 (6 eixos) |

**Documentos oficiais usados como fonte:**

- Wiki da placa: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06
- Datasheet ESP32-S3: https://files.waveshare.com/wiki/common/Esp32-s3_datasheet_en.pdf
- Technical Reference Manual: https://files.waveshare.com/wiki/common/Esp32-s3_technical_reference_manual_en.pdf
- Esquemático da placa: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf

---

## 3. Modos de energia do ESP32-S3

Da tabela "Components and Power Domains" do datasheet (Seção 3.2.1):

| Modo | CPU | Periféricos digitais | Circuitos wireless digitais | RF | Conexão BLE sobrevive? |
|---|---|---|---|---|---|
| **Active** | ON | ON | ON | ON | Sim |
| **Modem-sleep** | ON (clock pode reduzir) | ON | ON | Ligado periodicamente conforme necessário | **Sim** |
| **Light-sleep** | OFF | OFF (configurável) | ON (se configurado) | Ligado periodicamente conforme necessário | **Sim, com configuração específica** |
| **Deep-sleep** | OFF | OFF | OFF | OFF | **Não** — conexão cai, requer reconexão |

**Regra prática:** só **Modem-sleep** e **Light-sleep** preservam a conexão BLE. **Deep-sleep desliga o
rádio por completo** — mesmo que o firmware não chame explicitamente as funções de desabilitar BLE, a
conexão não sobrevive.

---

## 4. Estratégia recomendada: Light-sleep automático + BLE Modem-sleep

Mecanismo (documentação oficial ESP-IDF, "Sleep Modes"):

> Antes de entrar em deep-sleep ou light-sleep, a aplicação deve desabilitar Wi-Fi e Bluetooth. Conexões
> de Wi-Fi e Bluetooth **não** são mantidas em deep-sleep ou light-sleep, **a menos que** se habilite o
> modo modem-sleep de Wi-Fi/Bluetooth junto com o recurso de light-sleep automático — isso permite que o
> sistema acorde automaticamente quando o driver de BLE precisar, mantendo a conexão.

Ou seja, existe um modo **combinado**: modem-sleep do controlador BLE + light-sleep automático do sistema
(via `esp_pm` + FreeRTOS tickless idle). Quando habilitado:

1. O controlador BLE se torna uma fonte de wake-up do Power Management Unit (PMU).
2. O SoC entra em light-sleep no tempo ocioso entre eventos de conexão (definidos pelo *connection
   interval* + *slave latency* negociados).
3. O PMU acorda o chip automaticamente pouco antes de cada evento de conexão agendado.
4. Se o central escreveu na característica RX naquele evento, o HCI entrega o dado para o host NimBLE
   assim que o rádio acorda — o callback de GATT dispara normalmente.
5. O sistema volta a dormir até o próximo evento.

Isso é **automático** — não é necessário chamar `esp_light_sleep_start()` manualmente nem programar
wake-up por timer para "checar" mensagens. O agendamento da própria pilha BLE já cumpre esse papel.

**Deep-sleep está fora de escopo** para este projeto: mesmo sendo o modo de menor consumo, ele derruba
a conexão. Só usar deep-sleep se o produto puder tolerar reconexão (novo advertising + novo connect).

---

## 5. Consumo de energia esperado

Números oficiais do exemplo `esp-idf/examples/bluetooth/nimble/power_save` para o ESP32-S3:

| Estado | Corrente típica |
|---|---|
| Ativo (pico) | 240 mA |
| Modem-sleep | 17,9 mA |
| Light-sleep (clock de baixo consumo = main XTAL) | 3,3 mA |
| Light-sleep (clock de baixo consumo = XTAL externo 32 kHz) | **230 µA** |

A diferença entre usar o XTAL principal ou um cristal externo de 32,768 kHz como fonte do
`RTC_SLOW_CLK` é de mais de **10×** no consumo em light-sleep. Vale muito a pena confirmar se há um
cristal de 32 kHz disponível para o próprio ESP32-S3 (ver Seção 7).

---

## 6. Configuração do projeto (menuconfig / sdkconfig)

Baseado no exemplo oficial `bluetooth/nimble/power_save` (que por sua vez é derivado do `bleprph`),
adaptado para NUS. Caminho: `idf.py menuconfig`.

| # | Caminho no menuconfig | Ação |
|---|---|---|
| 1 | `Component config → Hardware Settings → RTC Clock Config → RTC clock source` | Selecionar fonte do clock RTC (ver Seção 7) |
| 2 | `Component config → Power Management → [*] Support for power management` | Habilitar |
| 3 | `Component config → FreeRTOS → Kernel → configTICK_RATE_HZ` | `1000` |
| 4 | `Component config → FreeRTOS → Kernel → [*] configUSE_TICKLESS_IDLE` | Habilitar |
| 5 | `Component config → FreeRTOS → Kernel → configEXPECTED_IDLE_TIME_BEFORE_SLEEP` | `3` |
| 6 | `Component config → PHY → [*] Power down MAC and baseband of Wi-Fi and Bluetooth when PHY is disabled` | Habilitar |
| 7 | `Component config → Bluetooth → Controller Options → MODEM SLEEP Options → [*] Bluetooth modem sleep` | Habilitar |
| 8 | `... → [*] Bluetooth Modem sleep Mode 1` | Habilitar |
| 9 | `... → Bluetooth low power clock` | Escolher fonte (XTAL externo 32k ou main XTAL) |
| 10 | `... → [*] power up main XTAL during light sleep` | Habilitar |
| 11 | `Component config → Bluetooth → Host` | Selecionar **NimBLE** |
| 12 (placa com PSRAM octal) | `Component config → ... → CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND` | Testar com e sem, medir corrente real |

Trecho de `sdkconfig.defaults` equivalente:

```ini
CONFIG_PM_ENABLE=y
CONFIG_FREERTOS_USE_TICKLESS_IDLE=y
CONFIG_FREERTOS_IDLE_TIME_BEFORE_SLEEP=3
CONFIG_ESP_PHY_MAC_BB_PD=y
CONFIG_BTDM_MODEM_SLEEP=y
CONFIG_BTDM_MODEM_SLEEP_MODE_1=y
CONFIG_BTDM_LOW_POWER_CLOCK_MAIN_XTAL=y   # ou EXT_32K_XTAL, se o board tiver cristal dedicado
CONFIG_BTDM_MODEM_SLEEP_MODE_1_XTAL_PU=y  # "power up main XTAL during light sleep"
CONFIG_BT_NIMBLE_ENABLED=y
```

> Os nomes exatos das flags `CONFIG_BTDM_*` podem variar entre versões do ESP-IDF — sempre confirme via
> `idf.py menuconfig` na versão que você está usando, em vez de copiar o `sdkconfig` literalmente.

No código, habilitar o PM com light-sleep automático:

```c
esp_pm_config_t pm_config = {
    .max_freq_mhz = 160,
    .min_freq_mhz = 40,
    .light_sleep_enable = true,
};
esp_pm_configure(&pm_config);
```

A partir daqui, **não** chame `esp_light_sleep_start()` manualmente — o FreeRTOS entra em tickless idle
e o PM decide quando dormir, respeitando locks que drivers (BLE, UART, etc.) mantêm enquanto precisam da
CPU ativa.

---

## 7. Considerações específicas de hardware da placa

1. **Cristal de 32 kHz para o próprio ESP32-S3.**
   O board tem um RTC dedicado (PCF85063, via I2C) para manter hora com a bateria — mas isso é um chip
   **separado**, com seu próprio cristal. Não confundir com o cristal de 32,768 kHz que o *ESP32-S3 em si*
   usaria nos pinos `XTAL_32K_P` / `XTAL_32K_N` (pinos 21/22) para o `RTC_SLOW_CLK` interno.
   **Ação:** abrir o esquemático da placa e verificar se esses pinos têm um cristal populado antes de
   configurar `Bluetooth low power clock` como "external 32kHz crystal". Se não houver, usar RC interno
   ou main XTAL dividido (menos preciso, mais consumo, mas funcional).

2. **PSRAM Octal SPI (variante R8) durante light-sleep.**
   Existe a opção de Kconfig `CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND`, voltada a vazamento de corrente
   da PSRAM durante sleep — análoga à opção de power-down de flash (`CONFIG_ESP_SLEEP_POWER_DOWN_FLASH`,
   ou a chamada `esp_sleep_pd_config(ESP_PD_DOMAIN_VDDSDIO, ESP_PD_OPTION_OFF)`). O ESP-IDF não garante
   power-down de flash/PSRAM em **todas** as condições de light-sleep.
   **Ação:** testar com e sem essa flag e **medir corrente real na placa** — PSRAM octal tende a ser o
   maior fator de divergência entre o número "de datasheet" (230 µA) e o que se mede na prática.

3. **Watchdogs continuam contando durante light-sleep.**
   O RTC Watchdog e os MWDTs (Main System Watchdog Timers) não pausam durante light-sleep. Se o intervalo
   de sleep for longo, ajustar os timeouts com folga para não resetar o chip por engano.

---

## 8. Arquitetura do serviço NUS (NimBLE)

### 8.1 UUIDs padrão do Nordic UART Service

| Elemento | UUID | Direção | Propriedade GATT |
|---|---|---|---|
| Service | `6E400001-B5A3-F393-E0A9-E50E24DCCA9E` | — | — |
| RX (central → ESP32) | `6E400002-B5A3-F393-E0A9-E50E24DCCA9E` | Central escreve | `WRITE` / `WRITE_NO_RSP` |
| TX (ESP32 → central) | `6E400003-B5A3-F393-E0A9-E50E24DCCA9E` | ESP32 notifica | `NOTIFY` |

### 8.2 Esqueleto do GATT server

```c
#include "host/ble_hs.h"
#include "host/ble_gatt.h"

static uint16_t tx_handle;

// UUIDs (little-endian, byte a byte)
static const ble_uuid128_t nus_svc_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                      0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);

static const ble_uuid128_t nus_rx_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                      0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);

static const ble_uuid128_t nus_tx_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
                      0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static int nus_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                             struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t len = OS_MBUF_PKTLEN(ctxt->om);
        uint8_t buf[64];
        ble_hs_mbuf_to_flat(ctxt->om, buf, sizeof(buf), &len);
        // TODO: processar payload recebido aqui.
        // Este callback dispara mesmo que o chip estivesse em light-sleep
        // no instante anterior — o wake automático já entregou o dado.
    }
    return 0;
}

static const struct ble_gatt_svc_def gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &nus_rx_uuid.u,
                .access_cb = nus_rx_access_cb,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &nus_tx_uuid.u,
                .access_cb = NULL,
                .val_handle = &tx_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 } // terminador
        },
    },
    { 0 } // terminador
};
```

### 8.3 Fluxo de inicialização (visão geral)

1. `nimble_port_init()`
2. Configurar `ble_hs_cfg` (callbacks de sync/reset)
3. `ble_gatts_count_cfg()` + `ble_gatts_add_svcs(gatt_svcs)`
4. Definir nome do dispositivo via `ble_svc_gap_device_name_set()`
5. Iniciar advertising conectável (`ble_gap_adv_start`) com modo `BLE_GAP_CONN_MODE_UND`
6. `nimble_port_freertos_init(host_task)` — task do host NimBLE
7. Chamar `esp_pm_configure()` com `light_sleep_enable = true` (Seção 6) **depois** que o stack BLE
   estiver de pé.

---

## 9. Parâmetros de conexão: interval, slave latency, timeout

Trade-off central do projeto:

| Parâmetro | Efeito ao aumentar | Efeito ao diminuir |
|---|---|---|
| **Connection interval** | Mais tempo dormindo entre eventos → menor consumo. Maior latência para receber cada mensagem. | Mais eventos por segundo → maior consumo. Recepção mais rápida. |
| **Slave latency** | Periférico pode "pular" eventos de conexão → economia extra. Atraso adicional para reagir a uma escrita. | Periférico responde a todo evento → menor atraso, menor economia. |
| **Supervision timeout** | Mais tolerante a falhas temporárias de rádio, mas demora mais para detectar desconexão real. | Detecta desconexão mais rápido, mais sensível a interferência. |

Negociação do lado periférico (ESP32-S3):

```c
struct ble_gap_upd_params params = {
    .itvl_min = 24,      // 24 * 1.25ms = 30ms
    .itvl_max = 40,      // 40 * 1.25ms = 50ms
    .latency = 4,         // pula até 4 eventos
    .supervision_timeout = 400, // 400 * 10ms = 4s
};
ble_gap_update_params(conn_handle, &params);
```

**Atenção:** quem decide o valor final é o **central**. Se o central for um telefone Android (ou outro
stack BLE de sistema operacional), políticas internas de economia de bateria do próprio SO podem
sobrepor o que foi solicitado. **Medir o intervalo efetivo negociado** (via sniffer BLE ou logando o
evento de conexão no próprio firmware) em vez de assumir que o valor pedido foi respeitado.

Para dados que não são latência-crítica (ex.: leituras periódicas de sensor a cada minutos), vale a pena
folgar bastante o intervalo e a slave latency. Para algo mais interativo, manter intervalo curto e aceitar
consumo maior.

---

## 10. Checklist de implementação

- [ ] Confirmar papel do ESP32-S3 (peripheral vs central) e ajustar Seção 8 se necessário
- [ ] Verificar esquemático: cristal 32 kHz nos pinos `XTAL_32K_P`/`XTAL_32K_N` do ESP32-S3
- [ ] Habilitar `CONFIG_PM_ENABLE` + tickless idle + BT modem-sleep + light-sleep no menuconfig
- [ ] Implementar GATT server NUS (RX write / TX notify)
- [ ] Configurar advertising conectável com nome de dispositivo reconhecível
- [ ] Chamar `esp_pm_configure()` após stack BLE inicializado
- [ ] Negociar/testar connection interval e slave latency adequados ao caso de uso
- [ ] Medir corrente real da placa em light-sleep (multímetro/analisador de energia)
- [ ] Testar com e sem `CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND`
- [ ] Validar recepção de mensagens do central durante light-sleep prolongado (não só em bancada com USB ligado)
- [ ] Confirmar comportamento de reconexão após perda de conexão (fora de alcance, central desligado, etc.)
- [ ] Ajustar timeout dos watchdogs para o tempo de sleep esperado

---

## 11. Armadilhas conhecidas

1. **Deep-sleep não é substituto** — quebra a conexão mesmo que pareça "só mais um nível de sleep".
2. **Prioridade da task NimBLE** — com tickless idle agressivo, garantir que a task do host NimBLE não
   fique esfomeada pelo scheduler.
3. **Central Android/desktop pode ignorar parâmetros de conexão solicitados** — sempre medir o valor
   real negociado.
4. **PSRAM octal pode manter consumo de light-sleep bem acima do valor de datasheet** se o workaround de
   leakage não estiver configurado corretamente — medir, não assumir.
5. **Watchdogs não pausam em light-sleep** — timeouts curtos demais podem resetar o chip durante um sleep
   longo legítimo.
6. **Testar com bateria real, não só USB** — o AXP2101 e o circuito de energia da placa podem introduzir
   comportamento diferente do que se vê alimentando via USB direto.

---

## 12. Referências

- Datasheet ESP32-S3 (Espressif, v1.6): https://files.waveshare.com/wiki/common/Esp32-s3_datasheet_en.pdf
- Technical Reference Manual ESP32-S3 (v1.2): https://files.waveshare.com/wiki/common/Esp32-s3_technical_reference_manual_en.pdf
- Wiki da placa Waveshare ESP32-S3-Touch-AMOLED-2.06: https://www.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06
- Esquemático da placa: https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf
- ESP-IDF — Sleep Modes (ESP32-S3): https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/sleep_modes.html
- Exemplo oficial NimBLE Power Save: https://github.com/espressif/esp-idf/blob/master/examples/bluetooth/nimble/power_save/README.md
- Exemplo base `bleprph` (GATT server NimBLE): https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/nimble/bleprph

---

*Documento gerado como referência de desenvolvimento — revisar valores de Kconfig contra a versão do
ESP-IDF efetivamente usada no projeto antes de aplicar em produção.*
