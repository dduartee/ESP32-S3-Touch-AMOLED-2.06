# Questionamentos sobre as Referências — 14_lvgl_ble_sleep

> Para agentes de pesquisa: investigar cada pergunta com profundidade, citando fontes (datasheet, TRM, código-fonte do IDF, esquemático da placa). Retornar respostas com referências exatas (arquivo, linha, URL).

---

## 1. Flag XTAL_PU — qual o nome correto no IDF 5.5.4?

**Contexto:** O Doc2 (`esp32s3-ble-nus-lightsleep-reference.md:147`) recomenda:
```ini
CONFIG_BTDM_MODEM_SLEEP_MODE_1_XTAL_PU=y
```
Mas o Doc2 também avisa (linha 151): *"Os nomes exatos das flags CONFIG_BTDM_* podem variar entre versões do ESP-IDF".*

O `sdkconfig` gerado pelo build com IDF 5.5.4 mostra:
```
# CONFIG_BT_CTRL_MAIN_XTAL_PU_DURING_LIGHT_SLEEP is not set
```

**Perguntas:**
- Qual é o nome **exato** da flag no Kconfig do IDF 5.5.4 para "power up main XTAL during light sleep"?
- Essa flag está sob `CONFIG_BT_CTRL_*` ou `CONFIG_BTDM_CTRL_*`?
- Existe dependência dela com `CONFIG_BT_CTRL_MODEM_SLEEP=y` ou é independente?
- O que acontece **de fato** se essa flag não estiver setada? O rádio BLE perde o clock? Ou o controller consegue usar o RTC slow clock para manter a conexão mesmo sem o XTAL?
- Verificar no código-fonte do IDF (`components/bt/controller/esp32s3/`) como o modem sleep usa o XTAL e qual o impacto de desligá-lo.

**Fontes para consultar:**
- `$IDF_PATH/components/bt/controller/Kconfig` ou `Kconfig.in`
- `$IDF_PATH/components/bt/controller/esp32s3/bt.c`
- `idf.py menuconfig` → Component config → Bluetooth → Controller Options → MODEM SLEEP Options

---

## 2. Cristal 32.768 kHz nos GPIO16/17 — a placa Waveshare tem?

**Contexto:** Ambos os docs enfatizam que usar cristal externo de 32kHz reduz consumo de ~3.3mA para ~230µA no light sleep (14×). Mas o Doc2 (`:171-179`) alerta para não confundir o cristal do RTC PCF85063 (chip I2C separado) com os pinos `XTAL_32K_P`/`XTAL_32K_N` (GPIO16/17) do ESP32-S3. O Doc1 (`:116`) diz: *"A documentação da Waveshare não confirma a presença de um cristal 32.768kHz onboard"*.

**Perguntas:**
- **Abrir o esquemático da placa** (`ESP32-S3-Touch-AMOLED-2.06.pdf`) e verificar se GPIO16 e GPIO17 têm um cristal de 32.768 kHz populado, ou se estão em NC (não conectado).
- Se não houver cristal, qual a opção recomendada para o `RTC_CLK_SRC`? `INT_RC` (RC interno) ou `EXT_CRYS` com os pinos flutuando?
- A flag `CONFIG_RTC_CLK_SRC_EXT_CRYS=y` **sem cristal físico** nos pinos causa boot fail, comportamento instável, ou simplesmente fallback silencioso para RC interno?
- Existe a possibilidade de usar o clock do PCF85063 (I2C) como fonte indireta para o RTC slow clock do ESP32-S3? (Provavelmente não, mas confirmar.)
- Qual o consumo real medido em light sleep **nesta placa específica** com as flags atuais (RC interno)?

**Fontes para consultar:**
- Esquemático: `https://files.waveshare.com/wiki/ESP32-S3-Touch-AMOLED-2.06/ESP32-S3-Touch-AMOLED-2.06.pdf`
- `$IDF_PATH/components/soc/esp32s3/rtc_clk.c`
- Datasheet ESP32-S3, Seção 3.2 (System Clock)

---

## 3. NimBLE — ordem de inicialização e PM locks

**Contexto:** O Doc2 (`:271-272`) diz para chamar `esp_pm_configure()` **depois** que o stack BLE estiver de pé. Nosso código faz `bt_nus_init()` → `pm_configure_auto_light_sleep()`. Mas `bt_nus_init()` internamente chama `nimble_port_freertos_init()` que cria uma task que roda `nimble_port_run()`.

**Perguntas:**
- `nimble_port_init()` já configura o BLE controller para modem sleep? Ou isso só acontece depois que o host NimBLE está rodando?
- O BLE controller adquire automaticamente um PM lock (`esp_pm_lock_acquire`) quando há conexão ativa, ou o lock é adquirido/manual?
- Se `esp_pm_configure()` for chamado **antes** do `nimble_port_freertos_init()`, o que quebra? O modem sleep não registra wake source? Ou funciona mas sem otimização?
- A task do host NimBLE (`nimble_port_run()`) tem prioridade padrão 4. A display_task tem prioridade 2 (menor). Com tickless idle, o scheduler poderia "esfomear" a task NimBLE? Qual é o padrão de `nimble_port_freertos_init` (stack size, priority)?
- Verificar no código do exemplo oficial `bluetooth/nimble/power_save` qual a ordem exata de init usada.

**Fontes para consultar:**
- `$IDF_PATH/examples/bluetooth/nimble/power_save/main/main.c`
- `$IDF_PATH/components/bt/host/nimble/nimble/nimble/src/nimble_port_freertos.c`
- `$IDF_PATH/components/bt/controller/esp32s3/bt.c` (procure por `esp_pm_lock` ou `modem_sleep`)

---

## 4. Watchdog durante light sleep — o TWDT realmente dispara?

**Contexto:** O Doc2 (`:189-191`) alerta que *"Watchdogs não pausam durante light-sleep"*. O `sdkconfig` atual mostra:
```
CONFIG_ESP_TASK_WDT_TIMEOUT_S=5
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU0=y
CONFIG_ESP_TASK_WDT_CHECK_IDLE_TASK_CPU1=y
```

**Perguntas:**
- O TWDT (`esp_task_wdt`) monitora as idle tasks das CPUs 0 e 1. Quando a CPU entra em light sleep, a idle task "congela". O TWDT interpreta isso como starvation e dispara?
- Ou o TWDT usa um timer do RTC que **também** pausa durante light sleep, efetivamente "pausando" o watchdog? (Isso seria o comportamento correto, mas o Doc2 afirma o contrário.)
- Testar empiricamente: compilar com `CONFIG_ESP_TASK_WDT_TIMEOUT_S=5` e forçar light sleep com a CPU dormindo por >5s. O chip reseta?
- Qual a diferença entre TWDT, MWDT, e RTC WDT no contexto de light sleep?
- O `CONFIG_ESP_TASK_WDT_PANIC` não está setado no nosso sdkconfig. Se o TWDT disparar sem PANIC, qual o comportamento? Apenas log de warning?

**Fontes para consultar:**
- `$IDF_PATH/components/esp_system/task_wdt/task_wdt.c`
- Technical Reference Manual ESP32-S3, Capítulo 10 (Low-power Management), Seção de Watchdog Timers
- `$IDF_PATH/examples/system/light_sleep/main/light_sleep_example_main.c`

---

## 5. AXP2101 PMU — impacto real no consumo em sleep

**Contexto:** Ambos os docs mencionam o AXP2101 superficialmente. O Doc1 (`:1217-1224`) tem uma seção placeholder sem código real. O Doc2 (`:337`) alerta: *"Testar com bateria real, não só USB — o AXP2101 e o circuito de energia da placa podem introduzir comportamento diferente"*.

**Perguntas:**
- O AXP2101 gerencia os rails de energia do ESP32-S3. Durante light sleep, quais rails continuam ligados por padrão?
- É possível desligar rails não utilizados (codec de áudio ES8311, IMU QMI8658, RTC PCF85063) via I2C para reduzir o consumo em sleep?
- A comunicação I2C com o AXP2101 funciona durante light sleep ou precisa de wake explícito?
- Existe biblioteca pronta para ESP-IDF (XPowersLib, Axp2101, etc.) ou precisa de I2C raw? Qual o endereço I2C?
- Quanto o próprio AXP2101 consome em modo de operação normal vs sleep?

**Fontes para consultar:**
- Datasheet AXP2101: `https://dl.sunlight modulator.com/axp2101/axp2101_datasheet_v1.3_en.pdf`
- Procurar por "axp2101 esp-idf" no GitHub para bibliotecas existentes
- `examples/esp-idf/12_watch_ble/main/main.c` (funções `pmu_read_reg`, `pmu_write_reg`)

---

## 6. PSRAM Octal e `CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND`

**Contexto:** O Doc2 (`:181-187`) menciona que PSRAM octal tende a ser o maior fator de divergência entre consumo teórico e real em light sleep, e recomenda testar com e sem a flag `CONFIG_ESP_SLEEP_PSRAM_LEAKAGE_WORKAROUND`.

**Perguntas:**
- O que essa flag faz exatamente no código? Ela configura os pinos da PSRAM para um estado específico durante sleep?
- Há conflito com `CONFIG_SPIRAM_FETCH_INSTRUCTIONS=y` e `CONFIG_SPIRAM_RODATA=y` (que colocam código e dados na PSRAM)? Se a PSRAM é "desligada" durante sleep, o código em SPIRAM causa crash ao acordar?
- Medições empíricas com esta placa (ESP32-S3R8, 8MB Octal PSRAM): qual o consumo em light sleep com e sem essa flag?
- A flag `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND` (flash) tem efeito similar — testar ambas.

**Fontes para consultar:**
- `$IDF_PATH/components/esp_hw_support/sleep_modem.c` ou `sleep_psram.c`
- `$IDF_PATH/components/esp_psram/esp_psram_impl_octal.c`

---

## 7. Parâmetros de conexão BLE — trade-offs para sleep

**Contexto:** Os docs divergem nos parâmetros recomendados:
- Doc1 (`:781-788`): intervalo 500-1000ms, latency 10, supervision 5s
- Doc2 (`:289-295`): intervalo 30-50ms, latency 4, supervision 4s
- Doc1 tem um bug interno: a seção 7.1 mostra que `supervision_timeout=5s` é insuficiente para `latency=10, interval=1000ms` (precisa de ~30s)

**Perguntas:**
- Para um dispositivo que recebe dados esporádicos (não latência-crítica), qual configuração maximiza o tempo de sleep mantendo a conexão estável?
- O `bt_nus.c` atual **não configura parâmetros de conexão** após connect — usa os defaults negociados pelo central. Isso significa que o central (smartphone) decide o intervalo. Um Android típico usa quais parâmetros para peripheral BLE?
- A fórmula `supervision_timeout > (1 + slave_latency) * max_interval * 2` é um requisito do BLE spec ou apenas boa prática? O que acontece se for violada?
- Como logar os parâmetros **efetivamente negociados** (não os solicitados) no ESP32-S3? O event `BLE_GAP_EVENT_CONN_UPDATE` traz os valores reais?

**Fontes para consultar:**
- Bluetooth Core Specification v5.0, Vol 6, Part B, Section 4.5 (Connection Update)
- `$IDF_PATH/components/bt/host/nimble/nimble/nimble/host/src/ble_gap.c` (procure por `BLE_GAP_EVENT_CONN_UPDATE`)
- Exemplo `bleprph` do NimBLE

---

## 8. LVGL port stop/resume — realmente necessário?

**Contexto:** Os exemplos `11_lvgl_sleep` e `12_watch_ble` chamam `lvgl_port_stop()` antes do display sleep e `lvgl_port_resume()` depois. Nosso código (`power_mgmt.c`) **não** faz isso. O revisor apontou como issue MEDIUM.

**Perguntas:**
- O que exatamente `lvgl_port_stop()` faz? Para o timer task do LVGL? Desabilita o handler de touch?
- Se NÃO chamarmos `lvgl_port_stop()` e o timer LVGL disparar enquanto o display está em MIPI sleep (comando 0x10 enviado), o que acontece na prática?
  - QSPI transaction falha com timeout? Timeout de quanto tempo?
  - Crash? Data corruption no framebuffer?
  - Ou o driver QSPI simplesmente retorna erro silencioso?
- Testar empiricamente: dormir o display sem `lvgl_port_stop()`, esperar alguns segundos, acordar. A UI volta normal?
- `lvgl_port_resume()` recria o timer task do zero? Ou só despausa? Se recria, o estado interno do LVGL (objetos, estilos, fontes) é preservado?

**Fontes para consultar:**
- `managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c:120` (implementação de `lvgl_port_stop`)
- `managed_components/espressif__esp_lvgl_port/src/lvgl9/esp_lvgl_port.c:108` (implementação de `lvgl_port_resume`)
- `examples/esp-idf/11_lvgl_sleep/main/main.c`
- `examples/esp-idf/12_watch_ble/main/main.c`

---

## 9. Flag `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` — removida no IDF 5.5.4?

**Contexto:** Nosso `sdkconfig.defaults` tem `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH=y`, mas o `sdkconfig` gerado não contém essa flag — em vez disso tem `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y`. O Doc1 (`:241`) usa `CONFIG_ESP_SLEEP_FLASH_LEAKAGE_WORKAROUND=y` (nome correto), então parece que houve rename.

**Perguntas:**
- Confirmar via `git log` no repositório do IDF quando `CONFIG_ESP_SLEEP_POWER_DOWN_FLASH` foi removida/renomeada.
- As duas flags têm exatamente o mesmo efeito? Ou `FLASH_LEAKAGE_WORKAROUND` é um superset/subset?
- Existe documentação de migração do IDF 4.x → 5.x que liste flags renomeadas?

**Fontes para consultar:**
- `$IDF_PATH/components/esp_hw_support/Kconfig.sleep`
- `$IDF_PATH/docs/en/migration-guides/` (release notes do IDF 5.x)

---

## 10. Auto light sleep + tickless idle — como confirmar que está funcionando?

**Contexto:** O Doc2 (`:72-96`) descreve o mecanismo combinado (auto light sleep + BLE modem sleep) como "automático" e diz para **não** chamar `esp_light_sleep_start()` manualmente. Mas como verificar que está realmente acontecendo?

**Perguntas:**
- Existe uma flag de debug (`CONFIG_PM_TRACE=y` ou `CONFIG_PM_PROFILING=y`) que loga quando a CPU entra/sai de light sleep?
- Como medir o tempo de residência em light sleep vs active? O `esp_pm_dump_locks()` mostra os locks ativos, mas e o tempo?
- A função `esp_sleep_get_wakeup_cause()` funciona com auto light sleep? Retorna o motivo do último wake (BLE controller, timer, GPIO)?
- O BLE controller aparece como wake source após um connection event? Como confirmar que o wake NÃO é por polling da display_task?
- Qual o overhead de entrar/sair de auto light sleep? (latência de wake-up)

**Fontes para consultar:**
- `$IDF_PATH/examples/system/light_sleep/main/light_sleep_example_main.c`
- `$IDF_PATH/components/esp_pm/pm_impl.c` (implementação do power management)
- Doc2, Seção 4 (descrição do mecanismo)

---

## Instruções para os Agentes

Cada pergunta acima é uma unidade de pesquisa independente. Para cada uma:

1. **Ler as fontes indicadas** (arquivos do IDF, esquemático, datasheets)
2. **Responder com precisão**, citando arquivo e linha ou URL específica
3. **Se a resposta não for conclusiva** (ex: flag varia entre versões), indicar como resolver a ambiguidade (ex: `idf.py menuconfig`, teste empírico)
4. **Retornar no formato:**
   ```
   ## Resposta N: [título da pergunta]
   **Conclusão:** [resposta direta]
   **Evidência:** [citação com fonte]
   **Ação recomendada:** [o que fazer no código/sdkconfig]
   ```
