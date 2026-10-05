# Diagrama de Conexiones - Supervisor de Bombas

## Resumen de Pines MCU (STM32U585)

| Función | Pin MCU | Arduino Pin | Descripción |
| --- | --- | --- | --- | |
| PLC Bomba 1 | PA0 | A0 | Entrada - Orden PLC B1 |
| PLC Bomba 2 | PA1 | A1 | Entrada - Orden PLC B2 |
| PLC Bomba 3 | PA2 | A2 | Entrada - Orden PLC B3 |
| Relé Bomba 1 | PB0 | D0 | Salida - Control relé B1 |
| Relé Bomba 2 | PB1 | D1 | Salida - Control relé B2 |
| Relé Bomba 3 | PB2 | D2 | Salida - Control relé B3 |
| Modo GEN/RED | PC0 | D3 | Entrada - Contacto seco inversor (pull-up) |
| Feedback B1 | PC1 | D4 | Entrada - Retorno aux B1 (pull-down) |
| Feedback B2 | PC2 | D5 | Entrada - Retorno aux B2 (pull-down) |
| Feedback B3 | PC3 | D6 | Entrada - Retorno aux B3 (pull-down) |
| Nivel ADC | PA4 | A4 | ADC - Sensor 4-20mA (divisor) |
| UART TX | PA9 | D7 | TX -> Linux RX |
| UART RX | PA10 | D8 | RX <- Linux TX |
| LED OK | PB13 | D13 | Verde - Sistema OK |
| LED GEN | PB14 | D14 | Amarillo - Modo Generador |
| LED FAULT | PB15 | D15 | Rojo - Fallo |

---

## Esquema Eléctrico

```text
                     ┌─────────────────────────────────────┐
                     │           PLC (Existente)           │
                     │  Salidas: Contacto seco 24VDC       │
                     └──────────────┬──────────────────────┘
                                    │ 24VDC
                ┌───────────────────┼───────────────────┐
                ▼                   ▼                   ▼
         ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
         │  Relay B1   │     │  Relay B2   │     │  Relay B3   │
         │  (10A, 24V) │     │  (10A, 24V) │     │  (10A, 24V) │
         └──────┬──────┘     └──────┬──────┘     └──────┬──────┘
                │                   │                   │
                ▼                   ▼                   ▼
         ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
         │ Contactor B1│     │ Contactor B2│     │ Contactor B3│
         │  Bobina 24V │     │  Bobina 24V │     │  Bobina 24V │
         │   80mA      │     │   80mA      │     │   80mA      │
         └──────┬──────┘     └──────┬──────┘     └──────┬──────┘
                │                   │                   │
                ▼                   ▼                   ▼
         ┌─────────────┐     ┌─────────────┐     ┌─────────────┐
         │   Bomba 1   │     │   Bomba 2   │     │   Bomba 3   │
         │  7.5 kW     │     │  7.5 kW     │     │  7.5 kW     │
         └──────┬──────┘     └──────┬──────┘     └──────┬──────┘
                │                   │                   │
         ┌──────┴──────┐     ┌──────┴──────┐     ┌──────┴──────┐
         │  Aux NO B1  │     │  Aux NO B2  │     │  Aux NO B3  │
         │  (Retorno)  │     │  (Retorno)  │     │  (Retorno)  │
         └──────┬──────┘     └──────┬──────┘     └──────┬──────┘
                │                   │                   │
                └───────────────────┼───────────────────┘
                                    ▼
                     ┌─────────────────────────────┐
                     │    Arduino UNO Q (MCU)      │
                     │  STM32U585 @ 160MHz         │
                     │  Entradas: PC1, PC2, PC3    │
                     └─────────────────────────────┘
```

## Detección de Modo

```text
Inversor/Grupo Electrógeno
Contacto seco:
- Cerrado (24V) = RED  → MCU PC0 = HIGH (pull-up)
- Abierto (0V)  = GEN  → MCU PC0 = LOW
```

## Sensor Nivel 4-20mA

```text
Sensor 4-20mA ──────[120Ω]────── GND
                        │
                        ▼
                   ADC PA4 (A4)
                   3.3V ref, 12-bit

4mA  → 0.48V  → ADC ~780  → 0%
20mA → 2.40V  → ADC ~3900 → 100%
```

## Comunicación MCU ↔ Linux

```text
MCU UART1 (PA9/PA10) ──────── Linux /dev/ttyACM0
115200 8N1

Protocolo: 12 bytes binarios
[0xAA][TYPE][BOMBA_ID][TIMESTAMP][PAYLOAD][CRC8][0x55]
```

---

## Detalle Relés Intermedios

```text
PLC 24VDC ──┬──▶ [Relé Intermedio] ──▶ Bobina Contactor
            │         │
            │      MCU Control
            │         │
            └─────▶ MCU Pin (PB0/1/2)
                    HIGH = Relé CERRADO (paso señal)
                    LOW  = Relé ABIERTO (bloqueo)

Fail-safe: MCU sin alimentación → Relé abre por muelle
```

---

## Lista de Materiales (BOM)

| Ítem | Cantidad | Descripción | Especificación |
| --- | --- | --- | --- | |
| 1 | 1 | Arduino UNO Q | STM32U585 + QRB2210 |
| 2 | 3 | Relés intermedios | 24VDC bobina, contacto 10A/250VAC, 1NO+1NC |
| 3 | 3 | Contactores potencia | 24VDC bobina 80mA, 3P 18.5A AC-3 (7.5kW) |
| 4 | 1 | Sensor nivel | 4-20mA, 2 hilos, rango según tanque |
| 5 | 1 | Resistencia shunt | 120Ω 1% 0.5W (para 4-20mA) |
| 6 | 1 | Fuente 24VDC | 2A mínimo (PLC + relés + MCU) |
| 7 | 1 | Caja IP65 | Para montaje industrial |
| 8 | - | Bornas, cableado | 1.5mm² señal, 2.5mm² potencia |

---

## Notas de Instalación

1. **Orden de cableado**: Intercalar relés EN SERIE con bobinas contactores
2. **Puesta a tierra**: Conectar GND MCU, PLC, relés, sensor a mismo potencial
3. **Separación**: Cableado señales (24VDC) separado de potencia (400VAC)
4. **Blindaje**: Cable sensor 4-20mA blindado, malla a GND en un solo extremo
5. **Verificación**: Antes de energizar, medir continuidad y aislamiento


