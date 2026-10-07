# AGENTS.md

Safety-relevant industrial controller: an Arduino UNO Q (STM32U585) sits
**between** a non-modifiable PLC and 3 x 7.5 kW pumps, to allow only 1 pump
when the generator is online. Two independent halves, no shared build:

- `mcu/` — C++17 firmware (PlatformIO, `mcu/platformio.ini`), real safety
  logic.
- `linux/` — FastAPI telemetry/dashboard + Telegram bot (Debian QRB2210),
  stdlib-only auth, SQLite alert/metric stores. Entry: `python -m app.main`.
- Docs and code comments are in Spanish. Keep new prose/comments Spanish.

## Commands

Verified working here (`pio`, `cppcheck`, `pylint`, `markdownlint`, `docker`
are **not installed**; only `g++`, `python3`).

```bash
make tests           # pytest + native C++ (43 python tests, ~0.3s)
make tests-py        # pytest only
make tests-native    # g++ builds mcu/src/current_protector.cpp and runs it
./test_venv/bin/pytest tests/test_auth.py -q  # single file (prebuilt venv)
./test_venv/bin/python tests/test_auth.py     # files have __main__ runners
```

- `make lint` and `make docs` **never fail** (`|| true` on every sub-command),
  so don't treat them as verification. CI is the real gate:
  `python -m pylint --errors-only linux.app` (from repo root),
  `cppcheck --enable=all --std=c++17 --suppress=missingIncludeSystem src/
  include/` (in `mcu/`), `markdownlint docs/*.md README.md`.
- `make lint`'s pylint step needs `linux/venv`, which does not exist locally
  (`make linux-install` creates it). Use `test_venv/` for anything Python.
- `make tests-coverage` needs `pytest-cov`, which is **not** installed.
- Run the app only from `linux/` cwd: `cd linux && python -m app.main`.
  `app/main.py` mounts `StaticFiles(directory="static")` and serves
  `static/index.html` by **relative** path, and modules use relative imports
  → breaks from repo root. Without a real MCU it still boots (prints "No se
  pudo conectar al MCU") and serves :8080.

## Traps

- **The wire protocol is duplicated in 3 places** and must stay in sync:
  `mcu/include/config.h` (`McuMessage`, `crc8`, `validar_mensaje`),
  `linux/app/state.py` (`MSG_SIZE = 11`, `struct.unpack('<BBBIHBB')`, CRC8
  poly 0x07 over `data[1:10]`), `mcu/include/comm_bridge.h`.
  Frame: `0xAA | type | bomba_id | u32 ts | u16 payload | crc8 | 0x55`.
- **`McuCommand` in `linux/app/models.py` (0x01–0x04) is dead code**, and the
  "Comandos Recibidos (Linux → MCU)" table in `docs/states.md` is stale. Real
  opcodes are `McuEvent` values: `0x40 MODO_CHANGE`, `0x10 STATE_CHANGE`,
  `0xFF ERROR` (payload `0xFFFF` = reset of emergencia + mantenimiento) — sent
  by `StateManager.set_*`, handled in `CommBridge::handleCommand`. Don't
  "fix" `state.py` to use `McuCommand`.
- **The 1-pump-in-GENERADOR limit is firmware-only**, in exactly one place:
  `StateMachine::puedeArrancar` (`mcu/src/state_machine.cpp`). No API, admin
  or otherwise, may override it — stated in `linux/app/auth.py`, README and
  `docs/security_protocols.md`. Relays are spring-open NO: unpowered MCU ⇒
  no pump starts.
- **`SUPERVISOR_SERIAL_PORT` is dead config.** It appears in `.env.example`
  and `docker-compose.yml` but nothing reads it; `main.py` does
  `StateManager()` with the hardcoded default port `/dev/ttyACM0`.
- **RBAC has one source of truth:** `COMMAND_REQUIRED_ROLE` in
  `linux/app/auth.py`. Fail-closed (no `SUPERVISOR_*_PASSWORD` ⇒ every command
  rejected), unknown command always denied, and every attempt (allowed or
  denied) is audited to JSONL at `SUPERVISOR_AUDIT_LOG`. Tokens are HMAC
  (`b64(payload).b64(sig)`), not JWT, despite docstrings saying "JWT".
- `docs/security_protocols.md` RBAC table claims remote `set_modo_generador`
  is forbidden even for admin; the code grants it to admin. It is a Phase-1
  planning doc — code wins.

## Testing quirks

- `tests/test_state_machine.py` and `tests/test_relay_logic.py` assert against
  in-file `MockStateMachine` / `MockRelayControl` plus `linux/app/models.py`.
  They do **not** cover `linux/app/state.py` or any C++. Changing real logic
  without updating those mocks proves nothing.
- Only `tests/native/test_current_protector.cpp` compiles real firmware code
  (`mcu/src/current_protector.cpp` against `mcu/include`). Copy that pattern
  for new firmware coverage. Header-only modules compile directly; anything
  including `Arduino.h` needs the stub header CI builds in
  `.github/workflows/ci.yml` (`firmware-syntax` job) — `pio` is unavailable
  locally.
- `linux/app/` has **no `__init__.py`** (namespace package). Tests use two
  import styles: `sys.path` → `linux/app` then `from models import ...` (only
  for modules without relative imports), vs `sys.path` → `linux` then
  `from app.auth import ...`. Follow the style already used in the file you
  edit; `state.py`/`alerts.py`/`auth.py` are `app.*`-only.

## Docs / markdown

- CI runs `markdownlint docs/*.md README.md` from the repo root, which picks up
  `.markdownlint.json`: MD013 at 80 cols but with `tables: false` and
  `code_blocks: false`. **Table rows and code-fence content are exempt** —
  Spanish tables here are 5–6 columns and cannot fit 80 cols, so don't try to
  wrap them; only prose, headings and lists must stay ≤80.
- Everything else is default: MD022 (blank lines around headings), MD031/MD032,
  MD040 (fence needs a language — use `text`), MD004 (dashes, not `*`),
  MD047 (single trailing newline), and **MD060** (table delimiter rows must
  be `| --- | --- |`, with spaces around the dashes). Git history is full of
  markdownlint-only fix commits; verify with the CI command above rather than
  `make docs`, which cannot fail.
- `docs/security_protocols.md` (SIF matrix, proof-test schedule PT-01..PT-08,
  SIL targets) is the authoritative safety doc. Keep README/`docs/` in sync
  with code when behaviour changes.

## Deploy

- systemd unit `linux/systemd/telemetry.service` hardcodes
  `WorkingDirectory=/home/administrador/opencode-test/linux` and `linux/venv`,
  runs as root with `ProtectSystem=strict`. Credentials go in a
  `systemctl edit telemetry` override — never in the unit file.
- Docker: `docker compose up -d` (prod), or add
  `-f docker-compose.dev.yml` (mounts `./linux/app` + `./linux/static`, sets
  dev passwords, disables healthcheck). `linux/Dockerfile` requires the build
  context at the **repo root**.
- CI runs on push/PR to `master` only (`.github/workflows/ci.yml`;
  `main.yml` is an empty tracked file — ignore it). No lint/test config
  exists beyond CI + Makefile.
