# Supervisor de Bombas - Arduino UNO Q

Sistema de supervisión para 3 bombas de agua (7.5 kW c/u) que intercepta las
salidas de un PLC no reprogramable para limitar el arranque a 1 sola bomba
cuando hay generador.

## Resumen de Funcionamiento

| Modo | Bombas Permitidas | Comportamiento |
|------|-------------------|----------------|
| **RED** | 3 (B1, B2, B3) | Passthrough total - relés cerrados |
| **GENERADOR** | 1 (solo B1) | Bloquea B2 y B3, permite solo B1 |
| **EMERGENCIA** | 0 | Todos los relés abiertos |
| **MANTENIMIENTO** | 0 | Todos los relés abiertos (modo local) |

## Presupuesto Eléctrico (por qué existe este sistema)

| Concepto | Valor |
|----------|-------|
| Cada bomba | ~30 A |
| Generador de emergencia | ~45 A |
| **Máximo en GENERADOR** | **1 bomba (30 A)** - 2 bombas = 60 A = apagón |
| PLC/medidor DSE 7320 | Corta TODO por sobrecarga si ve >45 A |

El DSE 7320 y el PLC **no se modifican**. El Arduino actúa como capa
intermedia: en GENERADOR solo permite B1, de modo que el DSE nunca vea el
intento de arrancar 2 bombas. Además, con **su propio CT** (pinza en la
salida del generador, sin tocar los sensores del DSE) vigila la corriente y
actúa **antes** que la protección del DSE:

| Señal | Umbral | Acción |
|-------|--------|--------|
| Aviso | ≥40 A durante 1 s | Alerta a personal |
| Trip | ≥42 A durante 3 s (time-overcurrent, tolera inrush ~2 s) | EMERGENCIA: relés abiertos, requiere reset de operador |
| Contactor pegado | ≥2 A con relés abiertos durante 1 s | Alerta de contactor soldado |
| CT inválido | fuera de rango durante 0.5 s | Alerta de sensor |

Los valores se calibran en sitio con pinza amperométrica
(`mcu/include/config.h`).

## Arquitectura

```text
┌─────────────────┐     24VDC contacto seco      ┌──────────────────┐
│      PLC        │ ───────────────────────────▶ │   Arduino UNO Q  │
│  (no tocable)   │                              │  (STM32U585 MCU) │
└─────────────────┘                              │  3 Relés en serie│
                                                 └────────┬─────────┘
                                                          │
                     ┌────────────────────────────────────┼────────────────┐
                     ▼                                    ▼                ▼
               ┌──────────┐                         ┌──────────┐      ┌──────────┐
               │ Contac.  │                         │ Contac.  │      │ Contac.  │
               │   B1     │                         │   B2     │      │   B3     │
               └──────────┘                         └──────────┘      └──────────┘
                    │                                    │                 │
                    ▼                                    ▼                 ▼
               ┌──────────┐                         ┌──────────┐      ┌──────────┐
               │  Bomba 1 │                         │  Bomba 2 │      │  Bomba 3 │
               │  7.5 kW  │                         │  7.5 kW  │      │  7.5 kW  │
               └──────────┘                         └──────────┘      └──────────┘
                    ▲                                    ▲                 ▲
                    │                                    │                 │
               Retorno aux (NO/NC)                 Retorno aux        Retorno aux
                    │                                    │                 │
                    └────────────────────────────────────┼─────────────────┘
                                                         ▼
                                                 Entradas digitales MCU
```

## Fail-Safe

- **Arduino sin alimentación / MCU caído**: Relés abren por muelle →
  **ninguna bomba arranca**
- **PLC ordena arranque pero no hay retorno aux**: Fault → relé abre
  (bloqueo hasta reset de operador)
- **Sobrecarga del generador (>42 A sostenido)**: EMERGENCIA →
  relés abiertos, requiere reset manual
- **Comunicación MCU-Linux perdida**: MCU sigue operando en modo autónomo
- **Reset de emergencia**: comando de operador (también limpia latch de fallos)

## Sensores: qué se toca y qué no

Los sensores conectados al DSE 7320 **no se modifican** (el equipo es
caro y no debe bloquearse). El Arduino usa sus propias señales:

| Señal | Fuente |
|-------|--------|
| Corriente generador | **CT propio** (pinza, no invasivo) → `PIN_CORRIENTE_ADC` |
| Modo RED/GENERADOR | Contacto seco inversor (paralelo, no serie) → `PIN_MODO_GEN` |
| Orden PLC por bomba | Contacto seco de salida PLC → `PIN_PLC_BOMBAx` |
| Retorno aux contactores | NO auxiliar de cada contactor → `PIN_FEEDBACKx` |

## Acceso Remoto (roles)

| Rol | Contraseña | Comandos permitidos |
|-----|-----------|---------------------|
| **operator** | `SUPERVISOR_OPERATOR_PASSWORD` | `reset_emergencia`, `set_mantenimiento`, `request_status` |
| **admin** | `SUPERVISOR_ADMIN_PASSWORD` | todo lo anterior + `set_modo_generador`, `trigger_emergencia` |

- **Fail-closed**: sin contraseñas definidas, **ningún** comando remoto se
  acepta (solo lectura).
- Tokens HMAC con expiración (8 h por defecto) vía `POST /api/auth/login`.
- **Audit log** (JSONL): cada intento, permitido o denegado, con rol y
  timestamp.
- El límite de **1 bomba en GENERADOR es regla fija de firmware**: no existe
  comando remoto (ni admin) que lo sobreescriba.
- Configurar en `linux/systemd/telemetry.service` o por entorno.

## Telemetría Sin Pérdidas

- Alertas persistidas en **SQLite local** (`SUPERVISOR_ALERTS_DB`) → no se
  pierden datos si cae la red o hay tormentas.
- **Deduplicación** de alertas (60 s) → no satura al personal ni el canal.
- **Cola de entrega pendiente** (store-and-forward): las alertas se marcan
  entregadas al confirmar el envío; si el canal está caído, se reenvían
  al volver. Lista para conectar Telegram/SMS/SMTP como siguiente paso.
- Telemetría de corriente del generador cada 1 s (evento `0x31`).

## Hardware Confirmado

- [ ] 3 contactores - bobina 24VDC, 80mA
- [ ] Salidas PLC: relé contacto seco 24VDC
- [ ] Detección modo: contacto seco inversor (1=RED, 0=GEN)
- [ ] 3 retornos auxiliares contactores (NO/NC)
- [ ] 3 relés intermedios 24VDC, contacto 10A
- [ ] 1 CT pinza corriente generador (propio - NO modifica el CT del DSE)

## Estructura del Proyecto

```text
/home/administrador/opencode-test
├── README.md
├── Makefile
├── .vscode/
│   ├── settings.json
│   └── tasks.json
├── mcu/                      ← Firmware STM32U585
│   ├── src/
│   ├── include/
│   └── platformio.ini
├── linux/                    ← Telemetría Debian (QRB2210)
│   ├── app/
│   ├── static/
│   ├── requirements.txt
│   └── systemd/
├── docs/
│   ├── wiring.md
│   ├── states.md
│   └── fail_safe.md
└── tests/
    ├── test_state_machine.py
    ├── test_relay_logic.py
    ├── test_auth.py
    ├── test_alerts_store.py
    └── native/                 ← Tests g++ de la lógica real del MCU
```

## Desarrollo

### MCU (STM32U585)

```bash
cd mcu
pio run -t upload    # Compila y flashea
pio device monitor   # Serial monitor
```

### Linux Side (QRB2210)

```bash
cd linux
python -m venv venv
source venv/bin/activate
pip install -r requirements.txt
python -m app.main             # FastAPI en :8080
```

Variables de entorno (recomendado en producción, ver systemd):

```bash
export SUPERVISOR_OPERATOR_PASSWORD=...   # rol operator (reset/mantenimiento)
export SUPERVISOR_ADMIN_PASSWORD=...      # rol admin (todo)
export SUPERVISOR_AUTH_SECRET=...         # firma de tokens
```

### Tests

```bash
make tests           # Python (pytest) + nativos (g++ - lógica real del MCU)
make tests-py        # solo pytest
make tests-native    # solo tests nativos del firmware
```

## Documentación

- [Cableado](docs/wiring.md)
- [Máquina de Estados](docs/states.md)
- [Fail-Safe](docs/fail_safe.md)

## Licencia

Proyecto interno - Uso industrial