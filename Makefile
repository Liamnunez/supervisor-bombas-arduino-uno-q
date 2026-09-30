# Makefile - Supervisor de Bombas Arduino UNO Q
# Uso: make <target>

.PHONY: help mcu-build mcu-flash mcu-monitor linux-install linux-run linux-service tests docs clean

# Default target
help:
	@echo "=========================================="
	@echo "  Supervisor de Bombas - Arduino UNO Q"
	@echo "=========================================="
	@echo ""
	@echo "Targets MCU (STM32U585):"
	@echo "  make mcu-build     - Compilar firmware"
	@echo "  make mcu-flash     - Flashear al MCU"
	@echo "  make mcu-monitor   - Monitor serie (115200 baud)"
	@echo "  make mcu-clean     - Limpiar build MCU"
	@echo ""
	@echo "Targets Linux (QRB2210 Debian):"
	@echo "  make linux-install - Instalar dependencias Python"
	@echo "  make linux-run     - Ejecutar telemetría (dev)"
	@echo "  make linux-service - Instalar systemd service"
	@echo ""
	@echo "Tests & Calidad:"
	@echo "  make tests         - Ejecutar todos los tests"
	@echo "  make lint          - Linting (cppcheck + pylint)"
	@echo ""
	@echo "Documentación:"
	@echo "  make docs          - Generar/validar docs"
	@echo ""
	@echo "Limpieza:"
	@echo "  make clean         - Limpiar todo"

# ==========================================
# MCU Targets (PlatformIO)
# ==========================================
mcu-build:
	@echo "[MCU] Compilando firmware..."
	cd mcu && pio run

mcu-flash:
	@echo "[MCU] Flasheando..."
	cd mcu && pio run -t upload

mcu-monitor:
	@echo "[MCU] Abriendo monitor serie (Ctrl+C para salir)..."
	cd mcu && pio device monitor -b 115200

mcu-clean:
	@echo "[MCU] Limpiando build..."
	cd mcu && pio run -t clean

# ==========================================
# Linux Targets (QRB2210)
# ==========================================
linux-install:
	@echo "[LINUX] Instalando dependencias Python..."
	cd linux && python3 -m venv venv && ./venv/bin/pip install --upgrade pip && ./venv/bin/pip install -r requirements.txt

linux-run:
	@echo "[LINUX] Iniciando telemetría (FastAPI en puerto 8080)..."
	cd linux && ./venv/bin/python app/main.py

linux-service:
	@echo "[LINUX] Instalando systemd service..."
	sudo cp linux/systemd/telemetry.service /etc/systemd/system/
	sudo systemctl daemon-reload
	sudo systemctl enable telemetry
	sudo systemctl start telemetry
	@echo "[LINUX] Service instalado. Ver logs: journalctl -u telemetry -f"

linux-logs:
	journalctl -u telemetry -f

# ==========================================
# Tests
# ==========================================
tests:
	@echo "[TESTS] Ejecutando tests Python..."
	cd tests && python3 -m pytest -v

tests-coverage:
	@echo "[TESTS] Con cobertura..."
	cd tests && python3 -m pytest --cov=../linux/app --cov-report=html

# ==========================================
# Linting
# ==========================================
lint:
	@echo "[LINT] MCU (cppcheck)..."
	cd mcu && cppcheck --enable=all --std=c++17 --suppress=missingIncludeSystem src/ include/ 2>/dev/null || true
	@echo "[LINT] Linux (pylint)..."
	cd linux && ./venv/bin/pylint app/ || true

# ==========================================
# Documentation
# ==========================================
docs:
	@echo "[DOCS] Validando markdown..."
	command -v markdownlint >/dev/null && markdownlint docs/*.md README.md || echo "markdownlint no instalado (npm i -g markdownlint-cli)"

# ==========================================
# Clean
# ==========================================
clean: mcu-clean
	@echo "[CLEAN] Limpiando Linux venv..."
	rm -rf linux/venv
	@echo "[CLEAN] Limpiando test cache..."
	rm -rf tests/__pycache__ tests/.pytest_cache tests/htmlcov
	@echo "[CLEAN] Listo"

# ==========================================
# Desarrollo completo (una sola vez)
# ==========================================
setup: mcu-build linux-install
	@echo "[SETUP] Proyecto listo para desarrollar"