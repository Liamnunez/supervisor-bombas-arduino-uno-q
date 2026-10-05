# Análisis HAZOP/LOPA - Supervisor de Bombas
## Fase 2: Análisis de Riesgos Detallado (Versión 1.0)

---

## 1. Identificación de Nodos

| Nodo ID | Descripción | Límites / Interfaces |
|---------|-------------|----------------------|
| **N-01** | Alimentación 24VDC | Fuente → MCU, relés, PLC I/O, sensor nivel |
| **N-02** | Detección Modo | Contacto seco inversor (ATS) → PC0 (pull-up) |
| **N-03** | Órdenes PLC | PLC DO (contacto seco 24V) → PA0, PA1, PA2 |
| **N-04** | Salidas Relés MCU | PB0, PB1, PB2 → relés intermedios → bobinas contactores |
| **N-05** | Retorno Feedback | Contactores aux NO → PC1, PC2, PC3 (pull-down) |
| **N-06** | Sensor Nivel | 4-20mA → PA4 (ADC 12-bit + divisor 120Ω) |
| **N-07** | Sensor Corriente | CT pinza generador → PA5 (ADC) |
| **N-08** | Comunicación MCU↔Linux | UART1 PA9/PA10 ↔ QRB2210 /dev/ttyACM0 |
| **N-09** | E-Stop Físico | Botón cableado directo → relés (solo producción) |
| **N-10** | Generador | Salida → CT pinza → PA5 (monitor) |

---

## 2. Guía de Desviación (Guidewords)

| Código | Guía | Significado |
|--------|------|-------------|
| **NO** | Ninguna / Ausencia | La función no ocurre cuando debería |
| **MORE** | Más / Exceso | Más cantidad, tiempo, frecuencia, etc. |
| **LESS** | Menos / Déficit | Menos cantidad, tiempo, frecuencia |
| **AS WELL AS** | Además | Función correcta + algo extra no deseado |
| **PART OF** | Parte de | Solo parte de la función se ejecuta |
| **REVERSE** | Inverso | Función opuesta o inversión |
| **OTHER THAN** | Otro que | Algo completamente distinto ocurre |

---

## 3. Matriz de Riesgo (Likelihood × Severity)

| | S1 (Negligible) | S2 (Menor) | S3 (Moderado) | S4 (Mayor) | S5 (Catastrófico) |
|---|----------------|------------|---------------|------------|-------------------|
| **L5 (Casi cierto)** | Bajo | Medio | Alto | Crítico | Crítico |
| **L4 (Probable)** | Bajo | Medio | Alto | Alto | Crítico |
| **L3 (Posible)** | Bajo | Bajo | **Medio** | Alto | Crítico |
| **L2 (Poco probable)** | Bajo | Bajo | Bajo | **Medio** | Alto |
| **L1 (Raro)** | Bajo | Bajo | Bajo | Bajo | **Medio** |

**Umbral acción:** Medio/Alto/Crítico → requieren salvaguarda adicional documentada.

---

## 4. Hojas de Trabajo HAZOP por Nodo

### N-01: Alimentación 24VDC

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo (L×S) | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------------|----------------------|
| **NO** | Pérdida 24VDC | Fuente falla, fusible, cable cortado | MCU off → relés abren (fail-safe), PLC off, sin telemetría | Relés NO + muelle (fail-safe pasivo) | L3×S4=Alto | Alarma "Fuente 24V perdida" en Linux, UPS opcional |
| **MORE** | Sobretensión \>28V | Regulador falla, transitorio | Destrucción MCU, relés, sensores | Diodo TVS en entrada MCU | L2×S5=Alto | Protección transitoria + fusible rápido |
| **LESS** | Subtensión 18-22V | Carga excesiva, cable largo | MCU reset, relés inestables | Brown-out detect MCU | L3×S3=Medio | Monitor voltaje en Linux (ADC libre) |
| **AS WELL AS** | Ruido/ripple \>200mV | Conmutadores, motores | Lecturas ADC erráticas, reset MCU | Filtrado SW + HW (capacitores) | L2×S2=Bajo | Capacitores 100µF + 100nF cerca MCU |
| **REVERSE** | Polaridad invertida | Cableado error | Destrucción MCU/sensores | Diodo polaridad entrada | L1×S5=Alto | Conector polarizado + diodo serie |

### N-02: Detección Modo (ATS → PC0)

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------|----------------------|
| **NO** | Contacto ATS no cierra (RED) | Contacto sucio, cable roto, ATS falla | MCU lee HIGH (pull-up) → modo RED aunque sea GEN | Pull-up interno + debounce 50ms | L2×S4=Alto | Test contacto ATS en PT-02 |
| **MORE** | Rebote contacto ATS | Contacto sucio, vibración | Cambios modo rápidos (chatter) | Debounce 5 lecturas @ 100Hz (50ms) | L2×S2=Bajo | Contacto sellado / sellado hermético |
| **LESS** | Contacto ATS no abre (GEN) | Contacto soldado, ATS falla | MCU lee LOW → modo GEN, pero RED real | Debounce + cross-check con Linux | L2×S4=Alto | Alerta "Modo HW ≠ ATS real" |
| **AS WELL AS** | Ruido en línea modo | EMC, cables paralelos | Cambios modo falsos | Pull-up fuerte (4.7k), blindaje, debounce | L2×S2=Bajo | Cable blindado, tierra un punto |
| **REVERSE** | Cableado invertido (NO/NC) | Error instalación | Modo inverso permanente | Test PT-02 obligatorio | L1×S4=Alto | Checklist instalación, etiquetado |
| **OTHER** | Falla pull-up interno MCU | MCU defectuoso | Lectura flotante → modo aleatorio | Pull-up externo 4.7k obligatorio | L1×S4=Alto | Pull-up externo obligatorio en PCB |

### N-03: Órdenes PLC (PA0, PA1, PA2) - **YA DOCUMENTADO EN states.md**

### N-04: Salidas Relés MCU (PB0, PB1, PB2)

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------|----------------------|
| **NO** | MCU no cierra relé | MCU crash, pin falla, driver GPIO | Bomba no arranca | Feedback timeout (SIF-02) detecta | L2×S3=Medio | Test PT-01 semestral |
| **MORE** | MCU cierra relé sin orden | GPIO latch, SW bug, latch-up | Bomba arranca sin demanda | Feedback mismatch (SIF-03) detecta | L2×S3=Medio | Watchdog IWDG 5s, revisión código |
| **LESS** | Relé abre prematuramente | Driver débil, carga inductiva | Bomba para inesperadamente | Anti-cruzamiento 100ms, feedback | L2×S2=Bajo | Snubber RC en relés |
| **AS WELL AS** | Relé cierra + chatter | Rebote mecánico, driver inestable | Desgaste contactor, vibración | Anti-cruzamiento 100ms mínimo | L2×S2=Bajo | Relés estado sólido (SSR) opcional |
| **PART OF** | Solo 1 de 3 relés falla | Driver individual falla | 1 bomba no controlada | Feedback individual por bomba | L2×S3=Medio | Test individual PT-02 |
| **REVERSE** | Cableado cruzado B1↔B2 | Error instalación | Bomba equivocada arranca | Feedback mismatch detecta | L1×S3=Medio | Etiquetado + test PT-02 |
| **OTHER** | Relé queda pegado cerrado | Contacto soldado, residuos | Bomba no para al abrir | SIF-03 detecta mismatch | L2×S4=Alto | Relés con contactos de plata, test PT-04 |

### N-05: Feedback Loop (Contactores Aux NO → PC1, PC2, PC3)

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------|----------------------|
| **NO** | Feedback no llega (FB=0) | Cable roto, aux NC, contactor no cierra | Timeout 2s → SIF-02 fault_count++ → EMERGENCIA | Pull-down + timeout 2s + fault_count | L3×S4=Alto | Alarma temprana \>1s (warning) |
| **MORE** | Feedback fantasía (FB=1 sin orden) | Cable cruzado, contacto auxiliar soldado | SIF-03 mismatch → EMERGENCIA global | Comparación PLC vs FB cada ciclo | L2×S4=Alto | Test PT-04 semestral |
| **LESS** | Feedback intermitente | Cable suelto, contacto sucio | Fault_count crece → EMERGENCIA | Debounce 20ms (2 ciclos) | L3×S3=Medio | Conectores con traba, sellado |
| **AS WELL AS** | Feedback + ruido | EMC, ground loop | Fault_count falso | Debounce 20ms + filtro SW | L2×S2=Bajo | Cable blindado, tierra estrella |
| **REVERSE** | Feedback invertido (NO vs NC) | Error cableado aux | Lectura inversa permanente | Test PT-04 obligatorio | L1×S4=Alto | Checklist cableado aux |

### N-06: Sensor Nivel 4-20mA (PA4 + 120Ω)

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------|----------------------|
| **NO** | Señal \<3.5mA | Cable cortado, sensor falla, 120Ω abierto | sensor_ok=false, alerta crítica | ADC \<600 → sensor_ok=false | L3×S3=Medio | Alarma "Sensor nivel fail" inmediata |
| **MORE** | Señal \>21mA | Cortocircuito sensor, falla transmisor | sensor_ok=false, alerta crítica | ADC \>4000 → sensor_ok=false | L3×S3=Medio | Alarma "Sensor nivel saturado" |
| **LESS** | Señal 3.5-4mA (baja real) | Nivel muy bajo, sensor deriva | Lectura 0% falsa | Rango válido 4-20mA | L2×S2=Bajo | Alarma "Nivel crítico \<10%" |
| **MORE** | Señal 20-21mA (alta real) | Nivel muy alto, sensor deriva | Lectura 100% falsa | Rango válido 4-20mA | L2×S2=Bajo | Alarma "Nivel crítico \>90%" |
| **AS WELL AS** | Ruido en señal | Ground loop, EMC, cable sin blindar | Lectura errática, alertas falsas | Promedio 16 muestras, filtro SW | L2×S2=Bajo | Cable blindado, malla a GND un extremo |
| **OTHER** | Sensor fuera rango 3.5-21mA | Sensor defectuoso, cableado error | sensor_ok=false, alerta | Validación rango 3.5-21mA | L3×S3=Medio | Test calibración 6 meses |

### N-07: Sensor Corriente Generador (PA5 - CT Pinza)

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------|----------------------|
| **NO** | CT desconectado/roto | Cable roto, CT abierto | Corriente = 0A siempre | ADC \< threshold → alerta | L3×S4=Alto | Test CT obligatorio PT-05 |
| **MORE** | Lectura \>42A sostenida | Sobrecarga real, 2+ bombas en GEN | SIF-04 trip \>3s → EMERGENCIA | Time-overcurrent 42A/3s | L2×S4=Alto | Calibración CT con pinza real |
| **LESS** | Lectura baja falsa | CT saturado, mal posicionado | No detecta sobrecarga real | Calibración CT obligatoria | L2×S4=Alto | Verificación CT + pinza real |
| **AS WELL AS** | Armónicos / distorsión | Cargas no lineales | Lectura RMS errónea | Medición RMS real (SW) | L2×S2=Bajo | CT clase 0.5 o mejor |
| **OTHER** | CT instalado al revés | Instalación error | Lectura negativa/invertida | Valor absoluto en SW | L1×S3=Medio | Flecha dirección en CT + test |

### N-08: Comunicación MCU ↔ Linux (UART1 PA9/PA10)

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------|----------------------|
| **NO** | UART desconectado/roto | Cable roto, conector suelto | Sin telemetría, sin comandos remotos | Linux: sin heartbeat \>10s → alerta | L3×S3=Medio | Alerta "Sin heartbeat MCU" en \<30s |
| **MORE** | Datos basura/ruido | EMC, baudrate error, framing error | Comandos espurios, basura en logs | CRC8 + framing 0xAA/0x55 | L2×S2=Bajo | Baudrate fijo 115200, sin auto-negociación |
| **LESS** | Datos truncados | Buffer overflow, buffer underrun | Comandos incompletos | Buffer circular + timeout | L2×S2=Bajo | Buffer 256 bytes + timeout |
| **AS WELL AS** | Datos corruptos pero CRC OK | Colisión bit flip raro | Comando erróneo ejecutado | CRC8 polinomio 0x07 (Dallas) | L1×S3=Medio | Reintento automático en app |
| **REVERSE** | TX/RX cruzados | Cableado invertido | Sin comunicación bidireccional | Test comunicaciones obligatorio | L1×S2=Bajo | Test loopback en puesta en marcha |
| **OTHER** | Linux caído/reiniciado | SW crash, kernel panic, OOM | MCU autónomo (modo local) | MCU opera autónomo sin Linux | L3×S2=Medio | systemd restart=always, watchdog Linux |

### N-09: E-Stop Físico (Producción)

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------|----------------------|
| **NO** | E-Stop no accionado | Botón falla, cable roto | No para en emergencia | Cableado directo (hardwired) a relés | L2×S5=Crítico | Test PT-06 semestral obligatorio |
| **MORE** | E-Stop activado falso | Vibración, rebote, error operador | Parada innecesaria | Botón con guarda, 2 posiciones | L2×S2=Bajo | Botón EN 418 tipo hongo, 2 posiciones |
| **LESS** | E-Stop no suelta | Mecanismo atascado | No puede resetear | Diseño mecánico robusto | L2×S3=Medio | Test PT-06 verifica reset |
| **OTHER** | Cableado E-Stop cortado | Daño mecánico, robo cable | E-Stop inoperativo | Supervisión continuidad cable (opcional) | L1×S5=Crítico | Monitor continuidad cable E-Stop |

### N-10: Generador (Salida → CT → PA5)

| Guía | Desviación | Causas Posibles | Consecuencias | Salvaguardas Existentes | Riesgo | Salvaguarda Adicional |
|------|------------|-----------------|---------------|------------------------|--------|----------------------|
| **NO** | Generador no arranca | Fallo motor, combustible, batería | Sin potencia → bombas paran | DSE 7320 gestiona arranque | L3×S5=Crítico | DSE 7320 autónomo, alarma local |
| **MORE** | Generador \>45A | Sobrecarga, 2+ bombas en GEN | DSE 7320 trip → todo para | SIF-04 trip \>42A/3s + DSE trip | L3×S5=Crítico | SIF-04 + DSE coordinados |
| **LESS** | Generador \< voltaje/frecuencia | Carga excesiva, motor débil | Contactores no cierran, bombas no arrancan | DSE 7320 protege | L3×S4=Alto | Alarma "Gen \<400V / \<48Hz" |
| **OTHER** | Generador asincrónico | Desincronismo, falla AVR | Daño bombas, contactores | Protecciones DSE 7320 | L2×S4=Alto | DSE 7320 protecciones completas |

---

## 5. Análisis LOPA (Layer of Protection Analysis)

### SIF-01: Limitar a 1 bomba en GENERADOR

| Capa | Descripción | Tipo | PFD | Comentario |
|------|-------------|------|-----|------------|
| 1 | BPCS (PLC) lógica "solo 1 en GEN" | Prevención | 0.1 | PLC puede fallar |
| 2 | **SIS (Arduino) - SIF-01** | Prevención | 0.01 | **SIL 1 target** |
| 3 | Relé seguridad (Pilz PNOZ) en serie B2/B3 | Prevención | 0.001 | SIL 3 capable |
| 4 | E-Stop físico / E-stop lógico | Mitigación | 0.1 | Última barrera |

**Riesgo residual:** 0.1 × 0.01 × 0.001 = 10⁻⁶ → **SIL 1 cumplido** (target 10⁻⁵..10⁻⁶)

### SIF-02: Feedback Timeout

| Capa | Descripción | Tipo | PFD |
|------|-------------|------|-----|
| 1 | BPCS: PLC supervisa contactor | Prevención | 0.1 |
| 2 | **SIS: Timeout 2s + fault_count** | Prevención | 0.01 |
| 3 | Relé seguridad abre relé | Prevención | 0.001 |

### SIF-04: Sobrecorriente Generador

| Capa | Descripción | Tipo | PFD |
|------|-------------|------|-----|
| 1 | DSE 7320 protección sobrecarga | Prevención | 0.01 |
| 2 | **SIS: CT + time-overcurrent 42A/3s** | Prevención | 0.01 |
| 3 | Relé seguridad abre todo | Prevención | 0.001 |

---

## 6. Análisis de Fallo de Causa Común (CCF - Beta Factor)

| Elemento Compartido | Beta (β) | Mitigación |
|---------------------|----------|------------|
| Fuente 24VDC común | 0.1 | Fuente redundante opcional |
| MCU único (STM32) | 0.05 | Watchdog + diseño robusto |
| Relés mismos modelo/lote | 0.1 | Lotes distintos, prueba individual |
| Cableado mismo canal | 0.05 | Separación física, blindaje |
| Firmware común | 0.02 | Code review, test unitarios, watchdog |

**Beta efectivo sistema:** ~0.1 (aceptable para SIL 1)

---

## 7. Matriz de Verificación (V-Model)

| Fase V | Actividad | Entregable | Criterio Aceptación |
|--------|-----------|------------|---------------------|
| **Requisitos** | Especificación SRS | `docs/security_protocols.md` | Trazabilidad SIF↔Req |
| **Diseño** | Arquitectura capas, HAZOP | `docs/hazop_lopa.md` | Revisión independiente |
| **Implementación** | Firmware + App Linux | Código + Tests | Cobertura \>90% |
| **Integración** | Prueba banco (HIL) | Informe HIL | Todos SIF funcionales |
| **Validación** | Puesta en marcha sitio | Checklist PT-01..PT-08 | 100% pass |
| **Operación** | Proof tests periódicos | Actas firmadas PDF | Archivo 5 años |

---

## 8. Matriz de Trazabilidad SIF ↔ HAZOP

| SIF | Nodos Afectados | HAZOP Refs | SIL Target | Proof Test |
|-----|-----------------|------------|------------|------------|
| SIF-01 | N-02, N-03, N-04 | N-02(MORE), N-03(MORE), N-04(MORE) | SIL 1 | PT-02 Anual |
| SIF-02 | N-03, N-04, N-05 | N-03(NO), N-05(NO) | SIL 1 | PT-03 Semestral |
| SIF-03 | N-04, N-05 | N-04(OTHER), N-05(MORE) | SIL 1 | PT-04 Semestral |
| SIF-04 | N-07, N-10 | N-07(MORE), N-10(MORE) | SIL 1 | PT-05 Anual |
| SIF-05 | N-06 | N-06(NO/MORE) | SIL 1 | PT-05 Anual |

---

## 9. Próximos Pasos (Fase 3+)

- [ ] **Fase 3:** Especificación Hardware (BOM final, esquemas, Pilz PNOZ, E-stop)
- [ ] **Fase 4:** OTA Firmware (Ed25519, rollback, A/B partitions)
- [ ] **Fase 5:** Incident Response / Escalation Matrix
- [ ] **Fase 6:** Competencias operadores / Manual operación

---

*Fin de Fase 2 - HAZOP/LOPA completado. 10 nodos analizados, 5 SIFs definidos, SIL 1 confirmado para pilotaje. Listo para Fase 3 (Hardware).*