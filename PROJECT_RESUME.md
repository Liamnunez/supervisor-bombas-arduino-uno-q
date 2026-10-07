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
- `docs/hazop_lopa.md` - **Fase 2**: HAZOP/LOPA, 10 nodos, CCF, trazabilidad SIF↔HAZOP
- `docs/hardware_spec.md` - **Fase 3**: BOM, cableado, PNOZ s4, E-stop, PT-01..PT-09
- `docs/escalation_matrix.md` - **Fase 5**: quién atiende qué, ventanas, contactos
- `docs/severity_levels.md` - **Fase 5**: INFO/WARNING/CRITICAL y respuesta requerida
- `docs/playbooks.md` - **Fase 5**: 7 escenarios paso a paso (PB-01..PB-07)
- `docs/incident_response.md` - **Fase 5**: declarar, contener, investigar, recuperar, revisar
- `docs/drills.md` - **Fase 5**: 5 simulacros + plantilla de acta
- `docs/ota_procedure.md` - **Fase 4**: OTA firmado, 3 pasos de riesgo, formato, PT-10..PT-15

### 🧪 Tests
- **43 pytest** (state machine, relay logic, auth, alerts store, models)
- **21 native C++** (current_protector: inrush ride-through, trip once, re-arm, welded, CT fault, millis wrap)
- **23 native C++** (pnoz_heartbeat: fail-safe idle, pulse width, cadencia, reset, millis wrap, corte de latido)
- **173 native C++** (trip_policy: ciclo de 3 intentos, edge vs estado, ventana de 15 min, cooldown, operatorReset, overflow guard)
- **83 native C++** (fw_image: bounds, anti-rollback, CRC/SHA-256, shortcuts, fail-closed sin backend)
- **112 native C++** (boot_slot + self_test: A/B, rollback, activación local, auto-test)
- **50 pytest** (incluye test_error_codes.py: semántica 0x6020/0x6021)
- **Todos pasan**: 441/441 ✅

### 🐳 Docker & CI
- `docker-compose.yml` (prod) + `docker-compose.dev.yml` (dev con live reload)
- `linux/Dockerfile` multi-stage (builder → runtime, non-root)
- GitHub Actions CI: test, lint (pylint/cppcheck/markdownlint), firmware-syntax, build-docs
- Tests locales: `make tests` → 441/441 ✅

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
| **1** | **Fase 4**: Firmware OTA seguro (Ed25519, rollback A/B) | 2-3h | - |
| **2** | **Fase 6**: Formación operadores / competencias | 1h | - |
| **4** | Completar placeholders de `escalation_matrix.md` con personal real | 1h | Datos de planta |
| **5** | Confirmar PNOZ s4 en **modo watchdog** en banco (PT-09) | - | Hardware |
| **6** | Verificar PB3 expuesto en el conector UNO Q | 15 min | Hardware |
| **7** | Mapa de flash real para particiones A/B (2 MB, dual-bank) | - | Hardware |
| **8** | Levantar historial de corriente real para validar umbrales 40/42/45 A | - | CT instalado |
| **9** | Docker test en hardware real (QRB2210) | 1h | Hardware |

## ✅ Fases Completadas

| Fase | Entregable | Commit |
|------|-----------|--------|
| 1 | `docs/security_protocols.md` | `ecc4c5a` |
| 2 | `docs/hazop_lopa.md` | `1d282b3` |
| 3 | `docs/hardware_spec.md` | `e4a6e1a` |
| 5 | 5 docs + heartbeat PNOZ (SIF-06) | `7642f3a` |
| - | Politica de trip auto-recuperacion x3/15min | `19efa47` |
| 4 | `fw_image` + `boot_slot` + `self_test` + `ota_procedure.md` | pendiente |

---

## 🔐 Decisiones de Seguridad Pendientes (Fase 1 acordado)

| Decisión | Acordado | Pendiente |
|----------|----------|-----------|
| **SIL Target** | SIL 1 para pilotaje | Validar con HAZOP/LOPA → ¿SIL 2? |
| **Hardware diversity** | Single MCU + safety relay (Pilz PNOZ) | Confirmar hardware |
| **Emergency reset** | Local keyed switch ONLY | Implementar E-stop físico |
| **Remote mode override** | Admin puede cambiar modo remoto (código gana) | Revisar: desactiva el interlock |
| **Safety relay** | Pilz PNOZ entre MCU y contactor | Comprar/instalar |
| **E-stop físico** | Botón cableado directo a relés | Cablear |
| **Watchdog PNOZ** | ✅ Aprobado Fase 5 — latido PB3→P1 | Verificar modo watchdog (PT-09) |
| **Trip policy** | ✅ Aprobado Fase 5 — auto-recuperar ×3/15min | ✅ Implementada (`trip_policy.cpp`) |
| **Carga del generador** | 1 bomba (~30 A) de capacidad | Verificar cargas auxiliares en sitio |
| **Pozo en overflow** | Proteger generador: 1 bomba antes que 0 | Revisar con operador |

> ⚠️ **OTA no activable todavía.** `fwVerifyImage()` rechaza TODA
> imagen porque no hay backend Ed25519 (`FW_OTA_ENABLED false`). Es
> fail-closed a propósito. Ver `docs/ota_procedure.md` §10 para el
> inventario de lo que falta.

> ✅ **Política de trip implementada.** `mcu/src/trip_policy.cpp` + test
> nativo propio (173 checks). `StateMachine::autoRecoverTrip()` sale de la
> emergencia de sobrecarga **sin** limpiar los faults de feedback de las
> bombas — solo el reset de operador los libera. El reset de operador también
> pone a cero los intentos de la política.

---

## 📊 CI Status Actual

| Job | Último Estado | Commit |
|-----|---------------|--------|
| **test** | ✅ Local 441/441 | pendiente push |
| **lint** | ✅ docs nuevos markdownlint-clean | pendiente push |
| **firmware-syntax** | ✅ verificado con stub CI | pendiente push |
| **build-docs** | ✅ | pendiente push |

> **Último push**: `e4a6e1a` - "Fase 3: Especificación hardware"

---

## 🚀 Comandos Útiles

```bash
# Tests
make tests              # 441 tests (50 pytest + 391 native C++)
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

1. **Push de Fase 4** y verificar CI green.
2. **Fase 4 paso 1**: firma en CI + flasheo físico. Captura casi toda la
   seguridad sin tocar el bootloader. Ver `docs/ota_procedure.md` §3.
3. **Con hardware**: confirmar PNOZ s4 en modo watchdog (PT-09) y
   verificar que PB3 está expuesto en el UNO Q.
4. **Validar en banco**: los umbrales 40/42/45 A y el enfriamiento de 60 s
   con corriente real. Los 60 s están elegidos por criterio de vida ños
   del motor, no medidos.

---

*Documento generado automáticamente. Actualizar tras cada fase completada.*
