# Respostas aos Questionamentos — 14_lvgl_ble_sleep

> Data: 2026-07-27  
> Baseado em: ESP-IDF v5.5.4, ESP32-S3 Datasheet, ESP32-S3 TRM, Schematic Waveshare, Bluetooth Core Specification v5.0

---

## Resposta 1: Flag XTAL_PU — qual o nome correto no IDF 5.5.4?

**Conclusão:** O nome correto no IDF 5.5.4 é `CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP`. Ela está sob `CONFIG_BT_CTRL_*`, não `CONFIG_BTDM_CTRL_*`. A flag `CONFIG_BTDM_MODEM_SLEEP_MODE_1_XTAL_PU` é um nome legado/deprecated. A flag depende de `CONFIG_BT_CTRL_MODEM_SLEEP=y` e `CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL` ou `CONFIG_BT_CTRL_LPCLK_SEL_MAIN_XTAL`.

**Evidência:**
- A documentação oficial do ESP-IDF v5.4 (e v5.5.4) lista a flag como `CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP` em: Component config → Bluetooth → Controller Options → MODEM SLEEP Options. citeweb_search:9#1
- A descrição oficial diz: *"If this option is selected, the main crystal will power up during light sleep when the low power clock selects an external 32kHz crystal but the external 32kHz crystal does not exist or the low power clock selects the main crystal."* citeweb_search:9#1
- O Kconfig da versão v4.4.3 já mostrava a mesma flag com o prefixo `CONFIG_BT_CTRL_`, confirmando que o rename de `BTDM_` para `BT_CTRL_` ocorreu há várias versões. citeweb_search:9#0

**O que acontece se não estiver setada:**
- Se o low power clock do BLE controller estiver configurado para usar o main XTAL (40MHz) ou um 32kHz externo que não existe, **o main XTAL será desligado durante light sleep**.
- O BLE controller perde seu clock de referência e **não consegue manter a conexão** durante light sleep. Resultado: conexão BLE cai ou o chip não entra em light sleep.
- Se o low power clock usar o RTC slow clock interno (150kHz RC), essa flag não é relevante porque o RC não precisa do XTAL.

**Ação recomendada:**
```ini
# No sdkconfig.defaults
CONFIG_BT_CTRL_MODEM_SLEEP=y
CONFIG_BT_CTRL_MODEM_SLEEP_MODE_1=y
CONFIG_BT_CTRL_LPCLK_SEL_EXT_32K_XTAL=y  # ou MAIN_XTAL
CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y  # ESSENCIAL se usar XTAL
```

---

## Resposta 2: Cristal 32.768 kHz nos GPIO16/17 — a placa Waveshare tem?

**Conclusão:** O esquemático da placa ESP32-S3-Touch-AMOLED-2.06 **mostra um cristal de 32.768kHz conectado**, mas ele está ligado ao **PCF85063 (RTC externo via I2C)**, não diretamente aos pinos `XTAL_32K_P`/`XTAL_32K_N` (GPIO16/GPIO17) do ESP32-S3. Os pinos GPIO16/GPIO17 do ESP32-S3 **não têm cristal de 32.768kHz dedicado** no schematic desta placa.

**Evidência:**
- O schematic da placa mostra "32.768KHz" próximo ao PCF85063 (RTC I2C), não aos pinos GPIO16/17 do ESP32-S3. citeweb_search:13#2
- O schematic da variante 1.75C mostra GPIO16/GPIO17 como "125_MCLK" e "GPIO17" genérico, sem cristal 32k. citeweb_search:13#3
- A placa 1.8" da Waveshare (similar) mostra cristal 32.768kHz em posição diferente, ligado ao RTC chip. citeweb_search:13#10
- O repositório oficial da Waveshare não menciona cristal 32kHz para o ESP32-S3 nos pinos GPIO16/17. citeweb_search:13#0

**Implicações:**
- **Sem cristal 32kHz nos GPIO16/17**, o ESP32-S3 usará o **RC interno de 150kHz** (`CONFIG_RTC_CLK_SRC_INT_RC`) como RTC slow clock.
- O consumo em light sleep será **~3.3 mA** (base current) em vez de ~230 µA.
- A flag `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` **SEM cristal físico** nos pinos causará:
  - Boot delay aumentado (o bootloader tenta detectar o cristal)
  - Fallback silencioso para RC interno após timeout de detecção
  - Log de warning: `"32 kHz clock not found, switching to internal 150 kHz oscillator"` citeweb_search:11#7
  - **Não causa boot fail**, mas o consumo não será otimizado.

**Não é possível** usar o clock do PCF85063 (I2C) como fonte para o RTC slow clock do ESP32-S3. O PCF85063 é um RTC independente; seu clock de saída (se houver) não está roteado para o ESP32-S3 como XTAL_32K.

**Ação recomendada:**
```ini
# sdkconfig.defaults — usar RC interno (placa sem cristal 32k nos GPIO16/17)
CONFIG_RTC_CLK_SRC_INT_RC=y
# NÃO usar: CONFIG_RTC_CLK_SRC_EXT_CRYS=y

# Para reduzir consumo sem cristal 32k:
CONFIG_PM_POWER_DOWN_CPU_IN_LIGHT_SLEEP=y  # reduz ~650µA no ESP32-S3
CONFIG_PM_SLP_DISABLE_GPIO=y               # reduz ~200-300µA
```

---

## Resposta 3: NimBLE — ordem de inicialização e PM locks

**Conclusão:** A ordem correta é: `nimble_port_init()` → `ble_svc_gap_init()`/`nus_init()` → `nimble_port_freertos_init()` → `esp_pm_configure()`. O BLE controller **não** adquire PM locks automaticamente; o modem sleep é gerenciado internamente pelo controller. A task do host NimBLE tem prioridade padrão que não esfomeia o scheduler.

**Evidência:**
- O exemplo oficial `nimble/power_save` mostra a ordem: `nimble_port_init()` → init de serviços → `nimble_port_freertos_init()` → `esp_pm_configure()`. citeweb_search:11#0
- O log do exemplo power_save mostra: `pm: Frequency switching config: [...] Light sleep: ENABLED` **depois** que o BLE host task já está rodando. citeweb_search:13#7
- O BLE controller gerencia o modem sleep internamente. Não há `esp_pm_lock_acquire` explícito no código do controller para conexão BLE — o wake source é registrado pelo controller de forma transparente. citeweb_search:9#1

**Detalhes técnicos:**
- `nimble_port_init()` inicializa o controller e o host stack, mas **não** inicia a task do host.
- `nimble_port_freertos_init()` cria a task do host com stack size e priority padrão. A priority é tipicamente **configurável** e não fixa em 4 — depende do `menuconfig` (`CONFIG_BT_NIMBLE_TASK_STACK_SIZE` e priority relacionada).
- Se `esp_pm_configure()` for chamado **antes** do `nimble_port_freertos_init()`, o sistema pode entrar em light sleep antes do host estar pronto, mas o controller já está inicializado e gerencia os eventos de rádio. A ordem recomendada é PM configure **depois** de tudo estar de pé.
- A display_task (priority 2) não esfomeia a task NimBLE porque:
  1. O scheduler FreeRTOS preemptivo garante que tasks de maior priority rodem
  2. A task NimBLE passa a maior parte do tempo bloqueada (`nimble_port_run()` usa event loop interno)
  3. O tickless idle só entra em light sleep quando **todas** as tasks estão bloqueadas

**Ação recomendada:**
```c
// Ordem correta em app_main()
1. nvs_flash_init();
2. nimble_port_init();           // Inicializa controller + host stack
3. ble_svc_gap_init();           // GAP
4. nus_init();                   // GATT services
5. nimble_port_freertos_init(ble_host_task);  // Cria task do host
6. esp_pm_configure(&pm_config); // Auto light sleep por último
```

---

## Resposta 4: Watchdog durante light sleep — o TWDT realmente dispara?

**Conclusão:** O TWDT **não dispara** durante auto light sleep gerenciado pelo FreeRTOS tickless idle. O TWDT é alimentado pelo MWDT (Main System WDT), que usa o clock do RTC. Durante light sleep, o RTC clock continua rodando, **MAS** o FreeRTOS tickless idle **pausa o tick do sistema** e o TWDT é alimentado (fed) automaticamente pelo kernel antes de entrar em sleep. Não há risco de TWDT timeout em auto light sleep bem configurado.

**Evidência:**
- A documentação oficial do ESP-IDF sobre watchdogs não menciona TWDT disparando durante light sleep automático. O TWDT monitora tasks que não yieldam; a idle task **yield naturalmente** ao entrar em light sleep. citeweb_search:11#2
- O issue #16186 mostra TWDT disparando na task `ble_ll_task` quando há **I2S task de alta priority** competindo, não por causa do light sleep em si. citeweb_search:11#0
- O issue #17473 mostra TWDT na `ble_ll_task` quando GPIO interfere com o BLE controller, novamente não relacionado ao light sleep. citeweb_search:11#3
- O fórum da Seeed mostra problemas de WDT em light sleep **manual** (`esp_light_sleep_start()`), não auto light sleep. citeweb_search:8#3

**Distinção entre watchdogs:**
| Watchdog | Clock | Comportamento no Light Sleep |
|----------|-------|------------------------------|
| **TWDT** | MWDT (APB/RTC derivado) | Pausa implicitamente quando o kernel entra em tickless idle; é fed antes do sleep |
| **IWDT** | MWDT (mesmo) | Similar ao TWDT |
| **RTC WDT** | RTC clock | Continua rodando, mas é usado para boot time, não para tasks |
| **XTWDT** | XTAL32K monitor | Só ativo se XTAL32K for usado; detecta falha do cristal |

**Teste empírico sugerido:**
```c
// Forçar light sleep por 10s (maior que TWDT timeout de 5s)
vTaskDelay(pdMS_TO_TICKS(10000));  // Com tickless idle ativo
// Resultado esperado: NENHUM reset. A CPU dorme, o TWDT não dispara.
```

**Ação recomendada:**
- Não é necessário desabilitar TWDT para usar auto light sleep.
- Se `CONFIG_ESP_TASK_WDT_PANIC` não estiver setado, um TWDT timeout (se ocorrer) imprime warning + backtrace e **continua executando** — não reseta o chip. citeweb_search:11#2
- Se houver resets WDT inesperados, investigar tasks de alta priority que não yieldam (ex: I2S, SPI a 80MHz), não o light sleep.

---

## Resposta 5: AXP2101 PMU — impacto real no consumo em sleep

**Conclusão:** O AXP2101 gerencia múltiplos rails de energia (BUCKs/LDOs) para o ESP32-S3 e periféricos. Durante light sleep, rails não utilizados (codec ES8311, IMU QMI8658, etc.) podem ser desligados via I2C (endereço **0x34**) para reduzir consumo. O próprio AXP2101 consome **<20µA em off/standby**, mas em operação normal o quiescent current é maior.

**Evidência:**
- O AXP2101 é listado como PMIC da placa no repositório oficial. citeweb_search:13#0
- O endereço I2C do AXP2101 é **0x34** (confirmado por scan I2C na placa). citeweb_search:13#1
- Datasheet do AXP2101: "Low Power Off Current: less than 20uA when the device is off". citeweb_search:11#8
- A Waveshare fornece exemplo `01_AXP2101` para ESP-IDF demonstrando leitura de bateria e controle de rails. citeweb_search:13#0

**Rails típicos do AXP2101:**
| Rail | Alimenta | Pode desligar em sleep? |
|------|----------|------------------------|
| ALDO1 | ESP32-S3 VDD3P3 | ❌ NÃO (mantém ESP vivo) |
| ALDO2 | Display digital | ✅ Sim (se display dormir) |
| ALDO3 | Touch/FT3168 | ✅ Sim |
| BLDO1 | Codec ES8311 | ✅ Sim |
| BLDO2 | IMU QMI8658 | ✅ Sim |
| DLDO1 | RTC PCF85063 | ⚠️ Cuidado (perde hora) |

**Comunicação I2C durante light sleep:**
- A I2C **funciona** durante light sleep se o periférico I2C não for power downed.
- Se `CONFIG_PM_POWER_DOWN_PERIPHERAL_IN_LIGHT_SLEEP=y`, a I2C pode ser desligada e precisa de re-inicialização após wake.
- O AXP2101 mantém seu estado interno durante light sleep (reguladores configurados permanecem como estavam).

**Bibliotecas disponíveis:**
- **XPowersLib** (Xiaojie): biblioteca Arduino/ESP-IDF popular para AXP2101
- **Waveshare BSP**: o componente `waveshare/esp32_s3_touch_amoled_2_06` inclui wrappers para AXP2101
- **I2C raw**: possível mas não recomendado (registros não documentados publicamente)

**Ação recomendada:**
```c
// Exemplo: desligar rail do codec antes do sleep
// Usar biblioteca XPowersLib ou I2C raw
uint8_t reg_val = 0;
i2c_master_read_from_device(I2C_NUM_0, 0x34, &reg_val, 1, 100);
// Modificar bit do rail específico e escrever de volta
```

---

## Resposta 6: PSRAM Octal e CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND

**Conclusão:** A flag `CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND` **não desliga a PSRAM**. Ela configura o pino **CS da PSRAM para pull-up interno** durante light sleep, evitando que o chip PSRAM entre em estado selecionado (CS baixo) quando os GPIOs ficam flutuantes/isolados. Isso previne **corrupção de dados** e **leakage current** de ~100-500µA. Não há conflito com `SPIRAM_FETCH_INSTRUCTIONS` porque a PSRAM permanece energizada — apenas o CS é controlado.

**Evidência:**
- Descrição oficial: *"All IOs will be set to isolate(floating) state by default during sleep. Since the power supply of PSRAM is not lost during lightsleep, if its CS pin is recognized as low level(selected state) in the floating state, there will be a large current leakage, and the data in PSRAM may be corrupted by random signals on other SPI pins."* citeweb_search:8#1
- Custo: *"increase the sleep current about 10 uA"* (do pull-up interno). citeweb_search:8#1
- A flag `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND` faz o mesmo para o Flash CS. citeweb_search:8#1

**Efeito com PSRAM Octal (ESP32-S3R8):**
- Sem a flag: CS flutua → PSRAM pode interpretar ruído como comandos SPI → corrupção + leakage de corrente
- Com a flag: CS é pull-upado para alto → PSRAM fica desselecionada, segura e com leakage mínimo
- A PSRAM **não é desligada** — seus dados (incluindo código em SPIRAM) permanecem intactos

**A flag `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND`:**
- Mesmo mecanismo para Flash CS
- **Obrigatória para módulos** (placas com Flash/PSRAM onboard) citeweb_search:8#1
- Pode ser omitida apenas se houver pull-up externo no pino CS

**Ação recomendada:**
```ini
# sdkconfig.defaults — ambas são essenciais para módulos com PSRAM
CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y
CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y
CONFIG_SPIRAM=y
CONFIG_SPIRAM_MODE_OCT=y
# Não há conflito com:
CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y
CONFIG_SPIRAM_RODATA=y
```

---

## Resposta 7: Parâmetros de conexão BLE — trade-offs para sleep

**Conclusão:** Para receber dados esporádicos com máxima economia, usar **intervalo 500-1000ms, latency 4-10, supervision timeout 30s**. A fórmula `timeout > (1 + latency) * interval_max * 2` é um **requisito da Bluetooth Core Specification** (Vol 6, Part B, Sec 4.5.2). Se violada, o central **rejeita** o Connection Parameter Update Request ou a conexão cai com erro `0x3B` (Unacceptable Connection Parameters). O Android típico inicia com intervalo ~30-50ms e timeout ~5s, mas aceita updates do peripheral.

**Evidência:**
- Apple Accessory Design Guidelines: `Interval Max * (Slave Latency + 1) <= 2 seconds` e `Interval Max * (Slave Latency + 1) * 3 < connSupervisionTimeout`. citeweb_search:12#9
- Bluetooth Core Spec: supervision timeout em unidades de 10ms, range 0x000A a 0x0C80 (100ms a 32s). citeweb_search:12#12
- Log real de ESP32-H2 mostra Android conectando com `conn_itvl=21` (26.25ms), `latency=0`, `supervision_timeout=72` (720ms), depois atualizando para valores maiores. citeweb_search:11#7
- O central (smartphone) decide os parâmetros finais, mas o peripheral pode solicitar updates via L2CAP Connection Parameter Update Request. citeweb_search:12#0

**Correção da fórmula do Doc1:**
```
Doc1 propõe: interval=1000ms, latency=10, timeout=5s
Fórmula: timeout > (1 + 10) * 1000ms * 2 = 22000ms = 22s

5s é INSUFICIENTE. Mínimo recomendado: 30s (3000 em unidades de 10ms)
```

**Parâmetros recomendados corrigidos:**
```c
struct ble_gap_upd_params params = {
    .itvl_min = 400,   // 500ms
    .itvl_max = 800,   // 1000ms
    .latency  = 4,     // pula até 4 eventos
    .supervision_timeout = 3000,  // 30s (OBRIGATÓRIO > 22s)
    .min_ce_len = 0,
    .max_ce_len = 0
};
```

**Como logar parâmetros efetivamente negociados:**
- O evento `BLE_GAP_EVENT_CONN_UPDATE` no NimBLE entrega os valores reais aceitos pelo central. citeweb_search:12#0
- Use `ble_gap_conn_find(conn_handle, &desc)` para ler `desc.conn_itvl`, `desc.conn_latency`, `desc.supervision_timeout`.

**Ação recomendada:**
```c
// No ble_gap.c, evento BLE_GAP_EVENT_CONN_UPDATE:
rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
ESP_LOGI(TAG, "Real: interval=%.2fms latency=%d timeout=%.1fs",
         desc.conn_itvl * 1.25f, desc.conn_latency,
         desc.supervision_timeout / 100.0f);
```

---

## Resposta 8: LVGL port stop/resume — realmente necessário?

**Conclusão:** `lvgl_port_stop()` e `lvgl_port_resume()` são **recomendados** quando o display entra em sleep mode (comando MIPI 0x10) porque o `esp_lvgl_port` mantém uma **task do LVGL** e um **timer periódico** (`lv_timer_handler()`) rodando. Se o display estiver em sleep e o timer disparar, o flush callback tentará enviar dados via QSPI para um display dormindo, causando **timeout de transaction** (não crash, mas delay de ~100-500ms até o timeout do QSPI driver).

**Evidência:**
- O `esp_lvgl_port` cria uma task dedicada do LVGL e gerencia o `lv_timer_handler()` automaticamente. citeweb_search:13#4
- O componente suporta "Power saving" como feature listada. citeweb_search:13#4
- O QSPI para display AMOLED (CO5300) usa `esp_lcd_panel_io_tx_color()`; se o display não responder (em sleep), a transaction completa com timeout ou erro. citeweb_search:13#5

**O que `lvgl_port_stop()` faz:**
- Pausa a task do LVGL (suspende ou deleta)
- Para o timer de `lv_timer_handler()`
- **Não destrói** objetos, estilos, fontes — o estado interno do LVGL é preservado na heap/PSRAM

**O que `lvgl_port_resume()` faz:**
- Recria/reinicia a task do LVGL
- Reinicia o timer
- O estado do LVGL (árvore de objetos) permanece intacto

**Teste empírico sugerido:**
```c
// 1. Dormir display SEM lvgl_port_stop()
esp_lcd_panel_disp_sleep_on(panel_handle);  // ou comando 0x10
vTaskDelay(pdMS_TO_TICKS(5000));  // esperar timer LVGL disparar
// Observar: possível delay/lag nos logs, timeout do QSPI

// 2. Dormir display COM lvgl_port_stop()
lvgl_port_stop();
esp_lcd_panel_disp_sleep_on(panel_handle);
vTaskDelay(pdMS_TO_TICKS(5000));
lvgl_port_resume();
// Observar: sem delays, UI retorna normalmente
```

**Ação recomendada:**
```c
void display_enter_sleep(void) {
    lvgl_port_stop();  // Pausa task LVGL
    esp_lcd_panel_disp_sleep_on(lcd_panel);  // Display sleep (0x10)
}

void display_wake_up(void) {
    esp_lcd_panel_disp_sleep_off(lcd_panel);  // Display wake (0x11)
    lvgl_port_resume();  // Retoma task LVGL
    lv_obj_invalidate(lv_scr_act());  // Força redraw completo
}
```

---

## Resposta 9: Flag CONFIG_ESP_SLEEP_POWER_DOWN_FLASH — removida no IDF 5.5.4?

**Conclusão:** A flag **NÃO foi removida** — ela ainda existe no IDF 5.5.4/6.0 como `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH`. No entanto, ela tem **restrições severas**: só funciona quando não há PSRAM (`CONFIG_SPIRAM` desativado) ou quando a PSRAM tem fonte de alimentação independente. Como a placa ESP32-S3-Touch-AMOLED-2.06 usa PSRAM onboard (8MB Octal), **esta flag é automaticamente desativada** pelo Kconfig.

**Evidência:**
- Documentação oficial v6.0: `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` existe em: Component config → Hardware Settings → Sleep Config. citeweb_search:9#2
- Descrição: *"Can only be enabled if there is no SPIRAM configured."* / *"Can only be enabled if there is no SPIRAM or SPIRAM has independent power supply"* citeweb_search:9#2
- A flag `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND` é a alternativa **sempre disponível** e recomendada quando há PSRAM. citeweb_search:8#1

**Diferença entre as flags:**
| Flag | Efeito | Restrição |
|------|--------|-----------|
| `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` | Desliga a alimentação do Flash | Sem PSRAM ou PSRAM com VDD independente |
| `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND` | Pull-up no CS do Flash | Sempre disponível; +10µA de consumo |

**Por que não aparece no seu sdkconfig:**
- Se `CONFIG_SPIRAM=y` está ativo, o Kconfig **desabilita automaticamente** `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` (dependência `depends on !SPIRAM || SPIRAM_POWER_SUPPLY_INDEPENDENT`).
- O `sdkconfig` gerado omite a linha (mostra como comentada: `# CONFIG_ESP_SLEEP_POWER_DOWN_FLASH is not set`).

**Ação recomendada:**
```ini
# NÃO tentar ativar (será ignorado pelo Kconfig por causa da PSRAM)
# CONFIG_ESP_SLEEP_POWER_DOWN_FLASH=y  ← INÚTIL com PSRAM

# Usar em vez disso:
CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y
CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND=y
```

---

## Resposta 10: Auto light sleep + tickless idle — como confirmar que está funcionando?

**Conclusão:** Há múltiplas formas de confirmar:

### 1. Logs de debug (`CONFIG_PM_TRACE=y` ou `CONFIG_ESP_SLEEP_DEBUG=y`)
```ini
CONFIG_PM_TRACE=y
CONFIG_ESP_SLEEP_DEBUG=y
```
Isso habilita logs internos do power management mostrando quando entra/sai de light sleep.

### 2. `esp_pm_dump_locks()` — mostra locks ativos
```c
esp_pm_dump_locks(stdout);  // Lista todos os PM locks e seu estado
```
Se não houver locks `ESP_PM_NO_LIGHT_SLEEP` ou `ESP_PM_CPU_FREQ_MAX` adquiridos, o sistema pode dormir. citeweb_search:12#2

### 3. `CONFIG_PM_PROFILING=y` — estatísticas de tempo
```ini
CONFIG_PM_PROFILING=y
```
Habilita `esp_pm_lock_get_stats()` que retorna tempo total em cada estado (active vs sleep). citeweb_search:12#2

### 4. `esp_sleep_get_wakeup_cause()` — funciona com auto light sleep
```c
esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
// Retorna: ESP_SLEEP_WAKEUP_TIMER, ESP_SLEEP_WAKEUP_GPIO, ESP_SLEEP_WAKEUP_BT, etc.
```
**Sim**, funciona com auto light sleep. Retorna o motivo do último wake. Para BLE, retorna `ESP_SLEEP_WAKEUP_BT` quando o controller BLE acordou a CPU. citeweb_search:8#2

### 5. Medição de corrente (método definitivo)
- Com multímetro em série com a bateria (modo µA)
- Consumo deve oscilar entre:
  - **~3-15 mA** durante eventos BLE (RX/TX)
  - **~0.8-3.3 mA** durante light sleep (depende do clock de sleep)
- Se o consumo ficar fixo em ~15-20mA, o light sleep não está ativo

### 6. Overhead de wake-up
- Latência típica de wake do auto light sleep: **< 1ms** (BLE controller wake)
- Latência de wake por timer: **~200-500µs**
- O ESP-IDF usa mecanismo preditivo de compensação de tempo (`vTaskStepTick()`). citeweb_search:12#2

**Ação recomendada:**
```c
// No loop principal, log periódico do wakeup cause
static void log_sleep_stats(void) {
    esp_sleep_wakeup_cause_t cause = esp_sleep_get_wakeup_cause();
    const char* cause_str = "UNKNOWN";
    switch(cause) {
        case ESP_SLEEP_WAKEUP_TIMER: cause_str = "TIMER"; break;
        case ESP_SLEEP_WAKEUP_GPIO:  cause_str = "GPIO"; break;
        case ESP_SLEEP_WAKEUP_BT:    cause_str = "BLE"; break;
        case ESP_SLEEP_WAKEUP_UART:  cause_str = "UART"; break;
        default: break;
    }
    ESP_LOGI(TAG, "Last wake cause: %s", cause_str);

    // Dump locks
    esp_pm_dump_locks(stdout);
}
```

---

## Resumo de Ações para o Projeto

| Item | Ação | Prioridade |
|------|------|------------|
| Cristal 32kHz | Usar `CONFIG_RTC_CLK_SRC_INT_RC=y` (placa não tem cristal nos GPIO16/17) | 🔴 Alta |
| XTAL_PU flag | Usar `CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP=y` | 🔴 Alta |
| BLE params | Corrigir supervision timeout para 30s (não 5s) | 🔴 Alta |
| Flash/PSRAM workaround | Manter ambas as flags ativas | 🟡 Média |
| AXP2101 | Investigar desligar rails não usados via I2C 0x34 | 🟡 Média |
| LVGL stop/resume | Implementar `lvgl_port_stop()` antes do display sleep | 🟡 Média |
| TWDT | Não precisa desabilitar; não dispara em auto light sleep | 🟢 Baixa |
| PM profiling | Ativar `CONFIG_PM_PROFILING=y` para medições | 🟢 Baixa |
| Power down flash | Não usar (incompatível com PSRAM onboard) | 🟢 Baixa |
