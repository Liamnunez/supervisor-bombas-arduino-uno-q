# Especificación Hardware - Supervisor de Bombas

## Fase 3: Hardware para Producción (Versión 1.0)

> Documento de ingeniería. Los valores eléctricos son **objetivos de diseño**
> pendientes de confirmar con las placas de datos reales (nameplates) de bombas
> y generador.

---

## 1. BOM Final (Bill of Materials)

| # | Cant | Descripción | Especificación | Proveedor sugerido |
| --- | --- | --- | --- | --- |
| 1 | 1 | Arduino UNO Q (STM32U585AI) | 160 MHz, 2 MB Flash, 786 KB RAM. Salida PB3 → PNOZ | Arduino / DigiKey |
| 2 | 3 | Relé intermedio | 24 VDC bobina, 10 A / 250 VAC, 1NO+1NC | Finder 40.52 / Omron G2R |
| 3 | 3 | Contactor de potencia | 24 VDC bobina 80 mA, 3P 18.5 A AC-3 | Schneider LC1D18 / Siemens 3RT2018 |
| 4 | 1 | **Relé de seguridad** | Pilz PNOZ s4, 24 VDC, 2 salidas, E-Stop + watchdog (P1/P2) | Pilz PNOZ s4 |
| 5 | 1 | **E-Stop físico** | Botón hongo EN 418, 2 NC + 1 NO, con llave | Siemens 3SB3 / EAO |
| 6 | 1 | Sensor de nivel | 4-20 mA, 2 hilos, IP68, rango según tanque | Vega / Endress+Hauser |
| 7 | 1 | Resistencia shunt | 120 Ω, 1 %, 0.5 W (0.25 W mín. disipación real) | Vishay / Yageo |
| 8 | 1 | CT pinza de corriente | 0-100 A, salida 4-20 mA, diámetro conductor ≥30 mm | CR Magnetics / Magnelab |
| 9 | 1 | Fuente 24 VDC | 2.5 A mín. (PLC + 3 relés + MCU + sensores) | Mean Well MDR-60-24 / Phoenix |
| 10 | 1 | Caja IP65 | ≥300×300×150 mm, riel DIN, panel | Rittal / Spelsberg / Fibox |
| 11 | - | Bornas | 2.5 mm² señal / 4 mm² potencia / PE verde-amarillo | Wago / Phoenix Contact |
| 12 | - | Glandes | M20 (señal), M25 (potencia), IP68 | Hummel / Lapp |
| 13 | - | Cableado señal | CY blindado 0.5 mm² (2-4 hilos) | Lapp / Helukabel |
| 14 | - | Cableado potencia | H07V-K 4 mm² (3F+N) | Lapp / Helukabel |

---

## 2. Topología de Conexión

```text
                     ┌──────────────────────────────────────┐
                     │      PLC EXISTENTE (BPCS, intocable)  │
                     │      DO contacto seco 24 VDC         │
                     └───────────────┬──────────────────────┘
                                     │ 3 señales de orden
        ┌────────────────────────────┼────────────────────────────┐
        │                            │                            │
        ▼                            ▼                            ▼
   ┌─────────┐                 ┌─────────┐                 ┌─────────┐
   │ PA0     │                 │ PA1     │                 │ PA2     │
   │ Orden B1│                 │ Orden B2│                 │ Orden B3│
   └────┬────┘                 └────┬────┘                 └────┬────┘
        │                            │                            │
        │        ┌───────────────────┴────────────────────┐       │
        │        │   ARDUINO UNO Q (STM32U585) — SIS      │       │
        │        │   entrada: PA0/PA1/PA2 (PLC)          │       │
        │        │   entrada: PC0 (modo ATS, pull-up)    │       │
        │        │   entrada: PC1/PC2/PC3 (feedback)     │       │
        │        │   entrada: PA4 (nivel 4-20 mA)        │       │
        │        │   entrada: PA5 (corriente CT)         │       │
        │        │   salida: PB0/PB1/PB2 (relés)         │       │
        │        │   UART: PA9/PA10 → Linux              │       │
        │        └───────────────────┬────────────────────┘       │
        │                            │                            │
        ▼                            ▼                            ▼
   ┌─────────┐                 ┌─────────┐                 ┌─────────┐
   │ PB0     │                 │ PB1     │                 │ PB2     │
   │ Relé B1 │                 │ Relé B2 │                 │ Relé B3 │
   └──┬───┬───┘                 └──┬───┬───┘                 └──┬───┬───┘
      │   │                        │   │                        │   │
      │   │            ┌───────────┴───┴────────────┐           │   │
      │   │            │  RELÉ DE SEGURIDAD PNOZ s4  │           │   │
      │   │            │  (en serie, fail-safe)     │           │   │
      │   │            │  13-14 → B1   23-24 → B2   │◀──────────┘   │
      │   │            │  23-24 ──▶ B2 y B3         │           │   │
      │   │            │  S11-S12/S21-S22 ← E-STOP  │           │   │
      │   │            └────────────────────────────┘           │   │
      │   │                        │                            │   │
      │   └────────────┬───────────┘                            │   │
      │                │                                        │   │
      ▼                ▼                                        ▼   ▼
 ┌─────────┐    ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐
 │ Bobina  │    │ Bobina  │   │ Bobina  │   │ Bobina  │   │ Bobina  │
 │ KM1     │    │ KM2     │   │ KM2     │   │ KM3     │   │ KM3     │
 │ 24 VDC  │    │ 24 VDC  │   │ (via    │   │ (via    │   │ 24 VDC  │
 └────┬────┘    └────┬────┘   │ 23-24)  │   │ 23-24)  │   └────┬────┘
      │              │         └────┬────┘   └────┬────┘        │
      │              │              │              │             │
      ▼              ▼              ▼              ▼             ▼
 ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐   ┌─────────┐
 │ Bomba 1 │   │ Bomba 2 │   │ Bomba 2 │   │ Bomba 3 │   │ Bomba 3 │
 │ 7.5 kW  │   │ 7.5 kW  │   │ 7.5 kW  │   │ 7.5 kW  │   │ 7.5 kW  │
 │ 380 VAC │   │ 380 VAC │   │ 380 VAC │   │ 380 VAC │   │ 380 VAC │
 │ 3F+N    │   │ 3F+N    │   │ 3F+N    │   │ 3F+N    │   │ 3F+N    │
 └─────────┘   └─────────┘   └─────────┘   └─────────┘   └─────────┘
      │              │              │              │             │
      │              │              └──────┬───────┴─────────────┘
      │              │                     │
      │              │        ┌────────────┴────────────┐
      │              │        │  Entradas digitales MCU  │
      │              │        │  PC1/PC2/PC3 (aux NO)    │
      │              │        │  pull-down               │
      │              │        └──────────────────────────┘
      │              │
      └──────────────┴──▶ 380 VAC 3F+N (colectores) → 3F+N → Generador
```text

---

## 3. Entradas y Protecciones

| Señal | Pin MCU | Tipo | Protección HW | Estado cable cortado |
| --- | --- | --- | --- | --- |
| Modo ATS | PC0 | Digital, **pull-up 10 kΩ externo** | RC 1 kΩ + 100 nF | HIGH → **RED** (NO restrictivo, ver nota) |
| Feedback B1 | PC1 | Digital, pull-down 10 kΩ | RC 1 kΩ + 100 nF | LOW → bomba parada |
| Feedback B2 | PC2 | Digital, pull-down 10 kΩ | RC 1 kΩ + 100 nF | LOW → bomba parada |
| Feedback B3 | PC3 | Digital, pull-down 10 kΩ | RC 1 kΩ + 100 nF | LOW → bomba parada |
| Nivel | PA4 | ADC 12-bit | 120 Ω shunt + TVS | 0 V → 0 % (fallback) |
| Corriente | PA5 | ADC 12-bit (**INPUT_ANALOG**) | Burden + TVS | 0 A → **ambiguo** (ver nota) |
| Orden PLC B1 | PA0 | Digital | RC + clamp | LOW → sin orden |
| Orden PLC B2 | PA1 | Digital | RC + clamp | LOW → sin orden |
| Orden PLC B3 | PA2 | Digital | RC + clamp | LOW → sin orden |

> ⚠️ **CABLE CORTADO EN PC0 = MODO RED. ES FAIL-OPEN.**
>
> Con pull-up, un cable cortado deja el pin flotando a HIGH, el firmware lo
> lee como RED y permite **las 3 bombas**. Una versión anterior de este
> documento afirmaba lo contrario ("LOW → GENERADOR, restrictivo") y era
> **incorrecto**: con pull-up el cable cortado nunca da LOW.
>
> ¿Por qué importa? El pin del ATS es la única fuente de verdad del límite de
> bombas. Un cable roto o un contacto sucio deja el sistema en RED con el
> generador conectado → 2-3 bombas → apagón del DSE. Ya está registrado
> como **N-02** en `docs/hazop_lopa.md` (L2×S4 = Alto).
>
> **Barrera obligatoria:** el pull-up 10 kΩ **externo en PCB** (no solo el
> interno del MCU) convierte el fallo del pull-up interno en un cable roto
> detectable, y `mode_detect.cpp` debe tratar un cambio a RED **visto mientras
> el generador está delivering** como condición a revisar, no como normal.
>
> Ningún software arregla esto: un pin flotante a HIGH *es* un nivel
> eléctrico válido. La única corrección es el pull-up externo y la
> verificación del contacto en PT-02.

---

## 4. Relé de Seguridad (Pilz PNOZ s4) — Wiring

```text
        ┌─────────────────────────────────────────────┐
        │          PILZ PNOZ s4 (24 VDC)              │
        │                                             │
        │  S11-S12 ◀──┐                               │
        │  S21-S22 ◀──┼── E-Stop NC contactos        │
        │             │   (2 canales, redundante)     │
        │             │                               │
        │  Y1-Y2  ◀──┴── Reset (botón o puente)      │
        │                                             │
        │  P1     ◀─── Latido del firmware (PB3)     │
        │             │   modo watchdog - 10ms/100ms  │
        │  P2     ──── +24 VDC (referencia de pulso) │
        │                                             │
        │  13-14  ──▶ salida seguridad 1 (S1)         │
        │  23-24  ──▶ salida seguridad 2 (S2)         │
        │                                             │
        │  A1  ──── +24 VDC                           │
        │  A2  ──── GND                               │
        └─────────────────────────────────────────────┘

   S1 (13-14) en serie con la alimentación de los relés B1
   S2 (23-24) en serie con la alimentación de los relés B2 y B3
```

**Cadena de seguridad real:**

```text
+24V → S1 (13-14) → bobina relé B1 → contacto NC/NO → GND
+24V → S2 (23-24) → bobina relé B2 → ...
+24V → S2 (23-24) → bobina relé B3 → ...
```

El PNOZ s4 **abre S1 y S2 simultáneamente** si:

- E-Stop se presiona (ambos canales abiertos)
- PNOZ detecta corto entre canales
- Fallo interno de diagnóstico
- El **latido del firmware se interrumpe** (modo watchdog, P1/P2)

Esto es **independiente del firmware**: aunque el MCU se cuelgue, el
E-Stop físico y el watchdog de latido abren la cadena sin que ningún código
tenga que participar.

### 4.1 Modo watchdog (latido del firmware)

Sin esta función el PNOZ solo protege contra el E-Stop. Con ella, un
firmware colgado también abre los relés.

| Parámetro | Valor | Nota |
| --- | --- | --- |
| Pin de salida | PB3 (`PIN_PNOZ_HEARTBEAT`) | Reposo LOW |
| Periodo del pulso | 100 ms (`PNOZ_PULSE_PERIOD_MS`) | Configurable |
| Ancho del pulso | 10 ms | 1 ciclo del loop de 100 Hz |
| Referencia | +24 VDC en P2 | Referencia del pulso |
| Reposo seguro | LOW (sin pulso) | MCU colgado antes del 1.º pulso → abre |

> ⚠️ **El ancho y periodo del pulso deben ajustarse al manual del PNOZ s4 y
> verificarse en banco.** El modo watchdog se activa por configuración
> interna del módulo (no por cableado), y el rango de pulsos aceptable
> depende de la configuración elegida. No cablear ni dar por servicio el
> sistema sin esa verificación — ver PT-09.
> ⚠️ **Verificar que PB3 esté expuesto en el conector del UNO Q** antes de
> cablear. Alternativas libres: PB12, PC4. Si el pin elegido no está
> expuesto, cambiar `PIN_PNOZ_HEARTBEAT` en `mcu/include/config.h`.
> **Límite conocido del método:** si el firmware se cuelga *durante* el
> pulso, el pin queda en HIGH y el PNOZ ve un pulso largo, no ausencia de
> pulso. Por eso el firmware fuerza el pin a LOW si el bloque de 100 Hz se
> retrasa más de `PNOZ_STALE_MS` (50 ms). Un cuelgue dentro de ese bloque no
> puede detectarse por software — solo el timeout del PNOZ lo cubre.

---

## 5. E-Stop Físico (EN 418, cableado directo)

```text
        ┌──────────────┐
        │  E-STOP      │   Botón hongo rojo, 22 mm
        │  2 NC + 1 NO │   Grillo + llave (opcional)
        │  Montaje:    │   Altura: 1.0-1.4 m
        │  frontal     │   Zona: pasillo acceso fácil
        └──────┬───────┘
               │
        ┌──────┴───────┐
        │  2 canales   │
        │  redundantes │   NC1 → S11-S12
        │  (2oo2)      │   NC2 → S21-S22
        └──────────────┘

   NO (1 de 3) → opcionalmente a entrada digital del MCU
                para registro en telemetría (no es función de seguridad)
```

**Requisito:** el NO puede ir al MCU para telemetría, pero la seguridad la dan
solo los 2 NC. Debe probarse el corte de telemetría sin afectar la parada.

---

## 6. Reglas de Cableado

| Regla | Detalle | Por qué |
| --- | --- | --- |
| **Separación de potencia/señal** | ≥200 mm entre cables 380 VAC y 24 VDC, o barriers metálicos | Acoplamiento inductivo/capacitivo |
| **Cables de señal** | CY blindado (malla), 0.5 mm², ≤10 m | Inmunidad EMC |
| **Malla de cables de señal** | **Un solo extremo** a GND (lado fuente, no lado MCU) | Evita loops de tierra |
| **Cables de potencia** | H07V-K 4 mm², ≥3F+N | Caída de tensión en 30 A |
| **Bornas** | 2.5 mm² señal, 4 mm² potencia, etiquetadas IEC | Mantenimiento |
| **Glandes** | IP68, M20 señal / M25 potencia | IP65 caja mantenida |
| **Tierra de caja** | Barra de PE, conexión a tierra planta | Seguridad eléctrica |
| **Cinta de fase** | Amarillo/naranja/rojo/verde-azul (L1/L2/L3/N) | Estándar/IEC 60445 |
| **Etiquetado** | Cada cable etiquetado en ambos extremos | Trazabilidad |

---

## 7. Requisitos PCB (si se diseña placa custom)

| Requisito | Especificación | Razón |
| --- | --- | --- |
| Capas | 4 capas (señal / GND / potencia / señal) | Retorno controlado, EMC |
| Material | FR4 TG170, 1.6 mm, 2 oz Cu | Mecánica y rigidez |
| Grosor de pista | 2 oz para pistas de potencia (>2 A) | Corriente |
| Clearing potencia-señal | ≥3 mm | Aislamiento |
| Anillo de tierra | Rule 5 mm alrededor de zona de potencia | Retorno |
| Test points | Todos los pines I/O accesibles | Diagnóstico |
| JTAG/SWD | Header 4 pines (SWDIO/SWCLK/3V3/GND) | Programación/debug |
| Protecciones | TVS 5V en entradas, RC en I/O, ESD | Robustez |

---

## 8. Proof Tests (PT-01 a PT-15) — Procedimientos

> Requisitos de `docs/security_protocols.md` §5. Ejecutar con personal
> cualificado. Registrar resultado firmado y archivado 5 años.

### PT-01 — Fallo de alimentación MCU (semestral)

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Registrar estado inicial (B1/B2/B3 running) | Estado conocido |
| 2 | Abrir breaker 24 VDC (desconectar MCU) | Relés abren en <100 ms |
| 3 | Verificar en panel: 3 bombas sin tensión | 0/3 bombas activas |
| 4 | Cerrar breaker 24 VDC | MCU arranca, relés permanecen abiertos hasta orden PLC |
| 5 | Restaurar estado previo | Operación normal |

**Firma:** _________________ Fecha: _________

---

### PT-02 — Modo Generador (semestral)

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Forzar contacto ATS a GENERADOR (abierto) | Estado → GENERADOR en <100 ms |
| 2 | Verificar relés | B1 passthrough, **B2/B3 abiertos** |
| 3 | Ordenar arranque de las 3 bombas desde PLC | Solo B1 arranca |
| 4 | Forzar contacto ATS a RED (cerrado) | Estado → NORMAL en <100 ms |
| 5 | Verificar relés | 3 bombas habilitadas |

**Firma:** _________________ Fecha: _________

---

### PT-03 — Feedback timeout (semestral)

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Desconectar cable feedback B1 (PC1) | Feedback B1 = LOW |
| 2 | Ordenar arranque B1 desde PLC | Relé B1 cierra |
| 3 | Esperar 2.5 s | Fault B1 encolado (fault_count++), relé abre |
| 4 | Verificar dashboard | Alerta crítica visible |
| 5 | Reset manual (admin) + reconectar feedback | Sistema operativo |

**Firma:** _________________ Fecha: _________

---

### PT-04 — Contactor pegado / mismatch (semestral)

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Conectar puente temporal: PC2 = HIGH sin orden PLC | Feedback B2 = HIGH sin orden |
| 2 | Esperar 2.5 s | Fault B2: mismatch detectado |
| 3 | Verificar relé B2 | **Abierto** (seguridad) |
| 4 | Retirar puente | Condición normal, mensaje persiste en audit log |

**Firma:** _________________ Fecha: _________

---

### PT-05 — Sobreccorriente generador (anual)

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Inyectar corriente >42 A (carga dummy o pinza) | Corriente medida >42 A |
| 2 | Mantener >3 s | Trip: EMERGENCIA activada |
| 3 | Verificar relés | Todos abiertos |
| 4 | Verificar dashboard + Telegram | Alerta crítica "SOBRECARGA" |
| 5 | Reducir corriente a < 5 A | Re-arm a los **60 s** (enfriamiento + `TRIP_RECOVER_AMPS`) |

**Firma:** _________________ Fecha: _________

---

### PT-06 — E-Stop físico (semestral)

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Botón de 3 bombas en marcha | Estado normal |
| 2 | Presionar E-Stop | **Todas las bombas paran en <50 ms** |
| 3 | Verificar relés PNOZ S1/S2 | Ambos abiertos |
| 4 | Intentar arrancar con E-Stop presionado | Imposible (relés no cierran) |
| 5 | Reset E-Stop + reset PNOZ | Re-arma manual, no automático |

**Firma:** _________________ Fecha: _________

---

### PT-07 — Watchdog MCU (anual)

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Compilar firmware con `while(true);` en loop() | Modo test |
| 2 | Flashear y dejar correr | MCU se cuelga |
| 3 | Esperar 5 s | IWDG resetea MCU |
| 4 | Verificar relés durante reset | Abiertos (fail-safe) |
| 5 | Restaurar firmware de producción | Verificado |

**Firma:** _________________ Fecha: _________

---

### PT-08 — Comunicación MCU-Linux (mensual)

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Desconectar UART / USB | Sin telemetría |
| 2 | Esperar 12 s | Alerta "Sin heartbeat MCU" |
| 3 | Verificar que MCU sigue operando | Relés y lógica de seguridad activas |
| 4 | Reconectar UART | Sincronización automática <5 s |
| 5 | Verificar dashboard | Estado correcto, sin gaps |

**Firma:** _________________ Fecha: _________

---

### PT-09 — Watchdog de latido PNOZ s4 (trimestral)

> ⚠️ **Este es el PT más importante de los nueve.** Si el PNOZ no está
> realmente en modo watchdog, toda la barrera de seguridad del proyecto se
> apoya únicamente en el firmware — que es exactamente lo que el diseño
> pretende evitar. Los pasos 5 y 6 son los que lo demuestran.

| Paso | Acción | Criterio de aceptación |
| --- | --- | --- |
| 1 | Verificar el pin de latido con osciloscopio | Pulso 10 ms / periodo 100 ms |
| 2 | Cortar alimentación 24 V del MCU | Relés abiertos por watchdog <500 ms |
| 3 | Restaurar 24 V | MCU arranca y rearma el PNOZ solo |
| 4 | Desconectar **solo** el cable P1-PB3 | Relés abiertos aunque el MCU esté vivo |
| 5 | Cortar el cable con el pin en reposo LOW | PNOZ detecta ausencia de pulso |
| 6 | Cortar el cable con el pin en HIGH (durante pulso) | PNOZ detecta pulso colgado |
| 7 | Cortocircuitar P1 a +24 V | PNOZ abre (pulso fijo = sin latido) |
| 8 | Reconectar y verificar | Rearme manual, no automático |

**Firma:** _________________ Fecha: _________

**Resultado paso 6:** __________ (si falla, documentar como riesgo abierto)

---

### PT-10 a PT-15 — Actualización de firmware (OTA firmado)

> Requieren bootloader A/B instalado. **No ejecutables hasta que exista.**
> Detalle del procedimiento en `docs/ota_procedure.md`.

| # | Prueba | Método | Criterio | Frecuencia |
| --- | --- | --- | --- | --- |
| **PT-10** | Firma corrupta | Alterar 1 byte de la firma | Rechazada; slot activo intacto | Trimestral |
| **PT-11** | Hash inconsistente | Recalcular CRC32 pero no SHA-256 | Rechazada en el hash | Trimestral |
| **PT-12** | Downgrade | Imagen con versión anterior | Rechazada (anti-rollback) | Trimestral |
| **PT-13** | Auto-test que falla | Firmware con umbral a 0 | Rollback automático | Semestral |
| **PT-14** | Corte durante transferencia | Cortar 24 V a mitad de descarga | Slot destino no corrupto; slot activo OK | Semestral |
| **PT-15** | Corte durante activación | Cortar 24 V al arrancar en TRIAL | Arranca en el slot confirmado | Semestral |

> **PT-14 y PT-15 son las críticas.** Un corte de luz a media escritura es
> el escenario real, no el teórico. Si el bootloader no sobrevive a eso, no
> hay A/B que valga y hay que volver al paso 1 (firma en CI + flasheo físico).

**Firma:** _________________ Fecha: _________

---

## 9. Checklist de Puesta en Marcha

### Pre-commissioning

- [ ] Esquemas eléctricos firmados por ingeniero competente
- [ ] Verificar partes físicas vs BOM (numeros de parte reales)
- [ ] Medir aislamiento: señal ≥1 MΩ, potencia según norma
- [ ] Verificar continuidad de PE (tierra)
- [ ] Verificar secuencia de fases en contactores
- [ ] Verificar torque de tornillos (contactos de potencia)

### Commissioning eléctrico

- [ ] Verificar 24 VDC en bornes (22-26 V)
- [ ] Verificar resistencia de bobinas: relés 24 V, contactores 24 V
- [ ] Verificar que el PNOZ abre todo con E-Stop presionado
- [ ] Verificar pull-up PC0 = HIGH por defecto (sin contacto ATS)
- [ ] Verificar pull-down PC1/PC2/PC3 = LOW por defecto

### Commissioning funcional

- [ ] PT-01 a PT-09 ejecutados y firmados (PT-10..PT-15 solo con A/B)
- [ ] PNOZ s4 confirmado en **modo watchdog** (no solo E-Stop)
- [ ] Ancho/periodo de pulso ajustados al manual del PNOZ s4
- [ ] PB3 (o pin sustituto) confirmado expuesto en el conector
- [ ] Calibración de CT de corriente (pinza real)
- [ ] Calibración sensor de nivel (4 mA / 20 mA)
- [ ] Verificar alertas Telegram llegan al chat correcto
- [ ] Verificar audit log registra todos los comandos
- [ ] Verificar fail-closed: passwords deshabilitadas → comandos rechazados

### Documentación

- [ ] Plan de mantenimiento actualizado con fechas
- [ ] Registros PT archivados (5 años)
- [ ] Lista de contactos de emergencia
- [ ] Procedimiento de respuesta a incidentes firmado

---

## 10. Registro de Cambios

| Vers | Fecha | Autor | Cambios | Aprobado |
| --- | --- | --- | --- | --- |
| 1.0 | 2026-10-05 | - | Especificación inicial Fase 3 | Pendiente |

---

*Fin de Fase 3 - Especificación Hardware. Siguiente: Fase 4 (OTA seguro) o
construcción del prototipo de bench.*
