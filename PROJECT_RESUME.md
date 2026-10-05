# Project Resume: Supervisor de Bombas - Arduino UNO Q
## Estado del Proyecto al 2026-10-05

---

## 📋 Resumen Ejecutivo

**Supervisor de Bombas** — Capa de seguridad industrial (Arduino UNO Q / STM32U585) que se interpone entre un PLC no reprogramable y 3 bombas de 7.5 kW cada una, para limitar a **1 sola bomba** cuando el generador de emergencia está activo (capacidad ~45A vs ~30A/bomba).

**Arquitectura:** Capa de seguridad (SIS) independiente del PLC (BPCS). El PLC sigue controlando; el Arduino supervisa y bloquea salidas inseguras.

---

## ✅ Estado Actual - Lo Completado

### 🏗️ Firmware MCU (STM32U585 / Arduino UNO Q)
- **Loop principal 100Hz** con watchdog hardware (IWDG 5s)
- **Máquina de estados** 4 estados: NORMAL, GENERADOR, EMERGENCIA, MANTENIMIENTO
- **5 SIFs (Safety Instrumented Functions)** implementadas:
  - SIF-01: Limitar a 1 bomba en modo GENERADOR (SIL 1)
  - SIF-02: Feedback timeout 2s (SIL 1)
  - SIF-03: Contactor soldado / mismatch (SIL 1)
  - SIF-04: Sobrecorriente generador >42A/3s (SIL 1)
  - SIF-05: Sensor nivel fuera de rango (SIL 1)
- **Protector de corriente** (time-overcurrent, tolera inrush ~2s)
- **Modo HW**: Pin PC0 (pull-up) = RED=HIGH, GEN=LOW con debounce 50ms
- **Fail-safe**: Relés NO + muelle → sin alimentación = bombas paradas
- **Protocolo MCU↔Linux**: 11 bytes binarios, CRC8, UART1 115200

### 🖥️ Telemetría Linux (QRB2210 / Debian)
- **FastAPI + WebSocket** en puerto 8080
- **Dashboard**: Gráficas Chart.js (nivel, corriente 24h), alertas, controles
- **SQLite persistente**: Alertas, métricas, runtime counters
- **Telegram Bot**: Store-and-forward, comandos /status /alerts /help
- **Autenticación**: HMAC tokens (8h), roles operator/admin, fail-closed
- **Audit log**: JSONL inmutable (SUPERVISOR_AUDIT_LOG)
- **CSV Export**: Historial alertas
- **Editor umbrales**: Admin puede cambiar umbrales en vivo

### 📚 Documentación
- `README.md` - Arquitectura, presupuesto eléctrico, desarrollo
- `docs/fail_safe.md` - Matriz fallos, capas protección, checklists
- `docs/states.md` - Máquina estados, transiciones, eventos, códigos error
- `docs/wiring.md` - Pinout, esquemas, BOM, notas instalación
- `docs/security_protocols.md` - **Fase 1**: SIF matrix, RBAC, ciberseguridad, proof tests
- `docs/security_protocols.md` - **Fase 2**: HAZOP/LOPA iniciado (N-03 ejemplo)

### 🧪 Tests
- **43 pytest** (state machine, relay logic, auth, alerts store, models)
- **21 native C++** (current_protector logic: inrush ride-through, trip once, re-arm, welded, CT fault, millis wrap)
- **Todos pasan**: 64/64 ✅

### 🐳 Docker & CI
- `docker-compose.yml` (prod) + `docker-compose.dev.yml` (dev con live reload)
- `linux/Dockerfile` multi-stage (builder → runtime, non-root)
- GitHub Actions CI: test, lint (pylint/cppcheck/markdownlint), firmware-syntax, build-docs
- Tests locales: `make tests` → 64/64 ✅

---

## 🔧 Arquitectura Técnica Clave

### Protocolo MCU ↔ Linux (3 lugares, must stay in sync)
```
mcu/include/config.h          → McuMessage struct, crc8()
linux/app/state.py            → MSG_SIZE=11, struct.unpack('<BBBIHBB')
mcu/include/comm_bridge.h     → CommBridge class
```
Frame: `0xAA | type | bomba_id | u32 ts | u16 payload | crc8 | 0x55`

### Seguridad - Regla de Oro
> **El límite de 1 bomba en GENERADOR es firmware-only** en `StateMachine::puedeArrancar()` (`mcu/src/state_machine.cpp`). **No existe API, admin ni override que lo desactive.**

### Fail-Safe Inherente
- Relés NO + muelle → MCU sin alimentación = relés abiertos = bombas paradas
- Pull-up PC0 (modo) → cable cortado = RED (seguro)
- Pull-down PC1-3 (feedback) → cable cortado = bomba parada

---

## ⚠️ Issues Conocidos / Deuda Técnica

| Área | Issue | Impacto |
|------|-------|---------|
| `SUPERVISOR_SERIAL_PORT` | Config muerta en `.env.example` y `docker-compose.yml` | Ninguno (hardcoded `/dev/ttyACM0`) |
| `McuCommand` en `models.py` | Código muerto (opcodes 0x01-0x04) | Confusión docs vs código |
| `McuCommand` table en `states.md` | Stale vs opcodes reales (0x40/0x10/0xFF) | Docs vs código |
| `docs/security_protocols.md` RBAC | Dice admin NO puede `set_modo_generador` | Código sí permite (admin) |
| `SUPERVISOR_SERIAL_PORT` en `.env.example` | Config muerta | Ninguno |
| `make lint` / `make docs` | `|| true` → nunca fallan | CI es la gate real |

---

## 📦 Estructura del Repositorio

```
/home/administrador/opencode-test/
├── mcu/                          # Firmware STM32U585
│   ├── src/                      # 8 .cpp files
│   ├── include/                  # 9 .h files
│   └── platformio.ini
├── linux/                        # Telemetría QRB2210
│   ├── app/                      # FastAPI app
│   ├── static/                   # Dashboard HTML/JS
│   ├── Dockerfile
│   └── requirements.txt
├── docs/                         # 5 .md files
├── tests/                        # 5 pytest + 1 native C++
├── .github/workflows/ci.yml      # CI pipeline
├── docker-compose.yml            # Prod stack
├── docker-compose.dev.yml        # Dev stack (live reload)
├── Makefile                      # Tests, lint, docker
└── PROJECT_RESUME.md             # Este archivo
```

---

## 🎯 Próximos Pasos Prioritarios

| Prioridad | Tarea | Esfuerzo | Bloqueante |
|-----------|-------|----------|------------|
| **1** | Verificar CI green (commit `ecc4c5a`) | 0 min | CI running |
| **2** | Completar **Fase 2: HAZOP/LOPA** (9 nodos restantes) | 2-3h | Decisión SIL 1 vs 2 |
| **3** | **Fase 3**: Hardware spec (relé seguridad Pilz PNOZ, E-stop, cableado) | 1-2h | Decisión hardware |
| **4** | **Fase 4**: Firmware OTA seguro (Ed25519, rollback) | 2-3h | - |
| **5** | **Fase 5**: Incident response / escalation matrix | 1h | - |
| **6** | **Fase 6**: Formación operadores / competencias | 1h | - |
| **7** | Limpiar deuda: `McuCommand` dead code, `SUPERVISOR_SERIAL_PORT` | 30m | - |
| **8** | Docker test en hardware real (QRB2210) | 1h | Hardware |

---

## 🔐 Decisiones de Seguridad Pendientes (Fase 1 acordado)

| Decisión | Acordado en Fase 1 | Pendiente |
|----------|-------------------|-----------|
| **SIL Target** | SIL 1 para pilotaje | Validar con HAZOP/LOPA → ¿SIL 2? |
| **Hardware diversity** | Single MCU + safety relay (Pilz PNOZ) | Confirmar hardware |
| **Emergency reset** | Local keyed switch ONLY | Implementar E-stop físico |
| **Remote mode override** | ❌ Solo local (hardwired ATS) | Confirmar |
| **Safety relay** | Pilz PNOZ entre MCU y contactor | Comprar/instalar |
| **E-stop físico** | Botón cableado directo a relés | Cablear |

---

## 📊 CI Status Actual

| Job | Último Estado | Commit |
|-----|---------------|--------|
| **test** | ✅ Local 64/64 | `ecc4c5a` |
| **lint** | ✅ Fixed (deps + pylint module) | `ecc4c5a` |
| **firmware-syntax** | ✅ Fixed (Arduino stub completo) | `ecc4c5a` |
| **build-docs** | ✅ | `ecc4c5a` |

> **Último push**: `ecc4c5a` - "Add Phase 1 security protocols" → CI running

---

## 🚀 Comandos Útiles

```bash
# Tests
make tests              # 64 tests (pytest + native)
make tests-py           # Solo pytest
make tests-native       # Solo native C++

# Lint (CI gate real)
cd linux && python -m pylint --errors-only linux.app
cd mcu && cppcheck --enable=all --std=c++17 --suppress=missingIncludeSystem src/ include/
markdownlint docs/*.md README.md

# Docker
docker compose up -d                    # Prod
docker compose -f docker-compose.yml -f docker-compose.dev.yml up -d  # Dev

# App local
cd linux && python -m app.main          # FastAPI :8080
```

---

## 📝 Próxima Acción Inmediata

**Esperar CI green** en commit `ecc4c5a` → luego **Fase 2: HAZOP/LOPA** completando 9 nodos restantes (N-01, N-02, N-04 a N-10) usando la plantilla de N-03 ya validada.

---

*Documento generado automáticamente. Actualizar tras cada fase completada.*
