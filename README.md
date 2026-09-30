# Supervisor de Bombas - Arduino UNO Q

Sistema de supervisión para 3 bombas de agua (7.5 kW c/u) que intercepta las salidas de un PLC no reprogramable para limitar el arranque a 1 sola bomba cuando hay generador.

## Resumen de Funcionamiento

| Modo | Bombas Permitidas | Comportamiento |
|------|-------------------|----------------|
| **RED** | 3 (B1, B2, B3) | Passthrough total - relés cerrados |
| **GENERADOR** | 1 (solo B1) | Bloquea B2 y B3, permite solo B1 |
| **EMERGENCIA** | 0 | Todos los relés abiertos |
| **MANTENIMIENTO** | 0 | Todos los relés abiertos (modo local) |

## Arquitectura

```
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

- **Arduino sin alimentación / MCU caído**: Relés abren por muelle → **ninguna bomba arranca**
- **PLC ordena arranque pero no hay retorno aux**: Fault → relé abre
- **Comunicación MCU-Linux perdida**: MCU sigue operando en modo autónomo

## Hardware Confirmado

- [ ] 3 contactores - bobina 24VDC, 80mA
- [ ] Salidas PLC: relé contacto seco 24VDC
- [ ] Detección modo: contacto seco inversor (1=RED, 0=GEN)
- [ ] 3 retornos auxiliares contactores (NO/NC)
- [ ] 3 relés intermedios 24VDC, contacto 10A

## Estructura del Proyecto

```
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
    └── test_relay_logic.py
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
python app/main.py   # FastAPI en :8080
```

### Tests
```bash
cd tests
python -m pytest -v
```

## Documentación

- [Cableado](docs/wiring.md)
- [Máquina de Estados](docs/states.md)
- [Fail-Safe](docs/fail_safe.md)

## Licencia

Proyecto interno - Uso industrial