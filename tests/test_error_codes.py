#!/usr/bin/env python3
"""
Tests del mapeo de errores del firmware (0x60xx) en el supervisor Linux.

Contexto: la política de trip (SIF-04, Fase 5) cambia el significado de
0x6001 - ya no requiere reset de operador, el firmware re-intenta solo - y
añade 0x6020 (re-arm automático) y 0x6021 (intentos agotados). Si el nivel
de alerta no se ajusta, cada tormenta en generador llenaría el canal de
críticos y el equipo dejaría de mirarlo.
"""

import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'linux'))

from app.state import MCU_ERROR_MESSAGES, MCU_ERROR_LEVELS


def test_trip_ya_no_dice_requiere_reset():
    """0x6001 debe decir que re-intenta solo, no que exige operador."""
    msg = MCU_ERROR_MESSAGES[0x6001]
    assert "re-intenta solo" in msg, msg
    assert "requiere reset" not in msg.lower(), msg


def test_codigos_nuevos_documentados():
    """0x6020 y 0x6021 deben existir en el mapa."""
    assert 0x6020 in MCU_ERROR_MESSAGES
    assert 0x6021 in MCU_ERROR_MESSAGES


def test_latch_exige_operador():
    """0x6021 es el único que de verdad exige a una persona."""
    assert "RESET DE OPERADOR" in MCU_ERROR_MESSAGES[0x6021].upper()


def test_rearm_es_info_no_critico():
    """Un re-arm automático no es crítico: si lo fuera, el canal se sature."""
    assert MCU_ERROR_LEVELS[0x6020] == "info"


def test_aviso_es_warning():
    assert MCU_ERROR_LEVELS[0x6010] == "warning"


def test_sin_entrada_es_critico():
    """Por defecto critical: fail-safe del nivel de alerta."""
    for code in (0x6001, 0x6002, 0x6003, 0x6021):
        assert code not in MCU_ERROR_LEVELS, code
        assert MCU_ERROR_LEVELS.get(code, "critical") == "critical"


def test_codigos_conocidos_no_tienen_nivel_inventado():
    """Todo código del mapa de mensajes debe tener un nivel válido."""
    for code in MCU_ERROR_MESSAGES:
        nivel = MCU_ERROR_LEVELS.get(code, "critical")
        assert nivel in ("info", "warning", "critical"), (code, nivel)


if __name__ == '__main__':
    tests = [
        test_trip_ya_no_dice_requiere_reset,
        test_codigos_nuevos_documentados,
        test_latch_exige_operador,
        test_rearm_es_info_no_critico,
        test_aviso_es_warning,
        test_sin_entrada_es_critico,
        test_codigos_conocidos_no_tienen_nivel_inventado,
    ]
    fallos = 0
    for t in tests:
        try:
            t()
            print(f"✓ {t.__name__}")
        except AssertionError as e:
            print(f"✗ {t.__name__}: {e}")
            fallos += 1
    print(f"\n{len(tests)} tests, {fallos} fallos")
    sys.exit(1 if fallos else 0)
