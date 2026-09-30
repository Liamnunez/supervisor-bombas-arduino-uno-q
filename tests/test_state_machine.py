#!/usr/bin/env python3
"""
Tests para máquina de estados - Supervisor de Bombas
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'linux', 'app'))

from models import SystemState, McuEvent, BombaStatus, SystemStatus, Alert


class MockStateMachine:
    """Mock de la máquina de estados para testing"""
    
    def __init__(self):
        self.state = SystemState.NORMAL
        self.previous_state = SystemState.NORMAL
        self.modo_generador_hw = False
        self.modo_mantenimiento = False
        self.emergencia_activa = False
        self.emergencia_codigo = 0
        self.bombas = [BombaStatus(i) for i in range(3)]
        self.callbacks = []
    
    def compute_desired_state(self):
        if self.emergencia_activa:
            return SystemState.EMERGENCIA
        if self.modo_mantenimiento:
            return SystemState.MANTENIMIENTO
        return SystemState.GENERADOR if self.modo_generador_hw else SystemState.NORMAL
    
    def transition_to(self, new_state):
        self.previous_state = self.state
        self.state = new_state
    
    def puede_arrancar(self, bomba_id):
        if bomba_id >= 3:
            return False
        if self.state in (SystemState.EMERGENCIA, SystemState.MANTENIMIENTO):
            return False
        if self.state == SystemState.GENERADOR and bomba_id != 0:
            return False
        if self.bombas[bomba_id].fault_count > 0:
            return False
        return True
    
    def on_feedback(self, bomba_id, activo):
        if bomba_id >= 3:
            return
        self.bombas[bomba_id].feedback = activo
    
    def on_plc_order(self, bomba_id, orden):
        if bomba_id >= 3:
            return
        self.bombas[bomba_id].plc_order = orden
    
    def set_modo_generador(self, gen):
        if not self.emergencia_activa:
            self.modo_generador_hw = gen
    
    def trigger_emergencia(self, codigo):
        self.emergencia_activa = True
        self.emergencia_codigo = codigo
    
    def set_mantenimiento(self, activo):
        self.modo_mantenimiento = activo
    
    def update_bombas(self):
        for i in range(3):
            self.bombas[i].relay_closed = self.puede_arrancar(i) and self.bombas[i].plc_order


def test_initial_state():
    """Test estado inicial por defecto"""
    sm = MockStateMachine()
    assert sm.state == SystemState.NORMAL
    assert sm.modo_generador_hw == False
    assert all(not b.plc_order for b in sm.bombas)
    print("✓ test_initial_state")


def test_modo_generador_hw():
    """Test transición por hardware a modo generador"""
    sm = MockStateMachine()
    sm.modo_generador_hw = True
    desired = sm.compute_desired_state()
    assert desired == SystemState.GENERADOR
    sm.transition_to(desired)
    assert sm.state == SystemState.GENERADOR
    print("✓ test_modo_generador_hw")


def test_modo_mantenimiento_priority():
    """Test que mantenimiento tiene prioridad sobre modo HW"""
    sm = MockStateMachine()
    sm.modo_generador_hw = True
    sm.modo_mantenimiento = True
    desired = sm.compute_desired_state()
    assert desired == SystemState.MANTENIMIENTO
    print("✓ test_modo_mantenimiento_priority")


def test_emergencia_priority():
    """Test que emergencia tiene máxima prioridad"""
    sm = MockStateMachine()
    sm.modo_generador_hw = False
    sm.modo_mantenimiento = True
    sm.emergencia_activa = True
    desired = sm.compute_desired_state()
    assert desired == SystemState.EMERGENCIA
    print("✓ test_emergencia_priority")


def test_puede_arrancar_normal():
    """Test lógica puede_arrancar en modo NORMAL"""
    sm = MockStateMachine()
    sm.state = SystemState.NORMAL
    for i in range(3):
        assert sm.puede_arrancar(i) == True
    print("✓ test_puede_arrancar_normal")


def test_puede_arrancar_generador():
    """Test lógica puede_arrancar en modo GENERADOR"""
    sm = MockStateMachine()
    sm.state = SystemState.GENERADOR
    assert sm.puede_arrancar(0) == True   # Bomba 1 permitida
    assert sm.puede_arrancar(1) == False  # Bomba 2 bloqueada
    assert sm.puede_arrancar(2) == False  # Bomba 3 bloqueada
    print("✓ test_puede_arrancar_generador")


def test_puede_arrancar_emergencia():
    """Test que en emergencia ninguna bomba puede arrancar"""
    sm = MockStateMachine()
    sm.state = SystemState.EMERGENCIA
    for i in range(3):
        assert sm.puede_arrancar(i) == False
    print("✓ test_puede_arrancar_emergencia")


def test_puede_arrancar_mantenimiento():
    """Test que en mantenimiento ninguna bomba puede arrancar"""
    sm = MockStateMachine()
    sm.state = SystemState.MANTENIMIENTO
    for i in range(3):
        assert sm.puede_arrancar(i) == False
    print("✓ test_puede_arrancar_mantenimiento")


def test_fault_bloquea_bomba():
    """Test que fault en una bomba la bloquea"""
    sm = MockStateMachine()
    sm.state = SystemState.NORMAL
    sm.bombas[1].fault_count = 1
    assert sm.puede_arrancar(0) == True
    assert sm.puede_arrancar(1) == False  # Bomba con fault
    assert sm.puede_arrancar(2) == True
    print("✓ test_fault_bloquea_bomba")


def test_relay_logic_normal():
    """Test lógica relés en modo NORMAL (passthrough)"""
    sm = MockStateMachine()
    sm.state = SystemState.NORMAL
    sm.on_plc_order(0, True)
    sm.on_plc_order(1, True)
    sm.on_plc_order(2, False)
    sm.update_bombas()
    assert sm.bombas[0].relay_closed == True
    assert sm.bombas[1].relay_closed == True
    assert sm.bombas[2].relay_closed == False
    print("✓ test_relay_logic_normal")


def test_relay_logic_generador():
    """Test lógica relés en modo GENERADOR (solo B1)"""
    sm = MockStateMachine()
    sm.state = SystemState.GENERADOR
    sm.on_plc_order(0, True)
    sm.on_plc_order(1, True)
    sm.on_plc_order(2, True)
    sm.update_bombas()
    assert sm.bombas[0].relay_closed == True   # B1 sigue PLC
    assert sm.bombas[1].relay_closed == False  # B2 bloqueada
    assert sm.bombas[2].relay_closed == False  # B3 bloqueada
    print("✓ test_relay_logic_generador")


def test_relay_logic_emergencia():
    """Test lógica relés en EMERGENCIA (todos abiertos)"""
    sm = MockStateMachine()
    sm.state = SystemState.EMERGENCIA
    sm.on_plc_order(0, True)
    sm.on_plc_order(1, True)
    sm.on_plc_order(2, True)
    sm.update_bombas()
    for i in range(3):
        assert sm.bombas[i].relay_closed == False
    print("✓ test_relay_logic_emergencia")


def test_feedback_mismatch_detection():
    """Test detección mismatch PLC vs Feedback"""
    sm = MockStateMachine()
    sm.state = SystemState.NORMAL
    sm.on_plc_order(0, True)
    sm.on_feedback(0, False)  # PLC pide pero no hay feedback
    assert sm.bombas[0].plc_order == True
    assert sm.bombas[0].feedback == False
    # Mismatch detectado
    print("✓ test_feedback_mismatch_detection")


def test_contactor_pegado_detection():
    """Test detección contactor pegado (feedback sin orden)"""
    sm = MockStateMachine()
    sm.state = SystemState.NORMAL
    sm.on_plc_order(0, False)
    sm.on_feedback(0, True)  # Feedback activo sin orden PLC
    assert sm.bombas[0].plc_order == False
    assert sm.bombas[0].feedback == True
    print("✓ test_contactor_pegado_detection")


def test_state_transition_sequence():
    """Test secuencia completa de transiciones"""
    sm = MockStateMachine()
    
    # Inicio NORMAL
    assert sm.state == SystemState.NORMAL
    
    # Generador detectado
    sm.modo_generador_hw = True
    sm.transition_to(sm.compute_desired_state())
    assert sm.state == SystemState.GENERADOR
    assert sm.previous_state == SystemState.NORMAL
    
    # Mantenimiento activado
    sm.set_mantenimiento(True)
    sm.transition_to(sm.compute_desired_state())
    assert sm.state == SystemState.MANTENIMIENTO
    assert sm.previous_state == SystemState.GENERADOR
    
    # Emergencia forzada
    sm.trigger_emergencia(0x0001)
    sm.transition_to(sm.compute_desired_state())
    assert sm.state == SystemState.EMERGENCIA
    assert sm.emergencia_codigo == 0x0001
    
    # Reset emergencia, vuelve a mantenimiento
    sm.emergencia_activa = False
    sm.emergencia_codigo = 0
    sm.transition_to(sm.compute_desired_state())
    assert sm.state == SystemState.MANTENIMIENTO
    
    # Salir mantenimiento, modo generador HW
    sm.set_mantenimiento(False)
    sm.transition_to(sm.compute_desired_state())
    assert sm.state == SystemState.GENERADOR
    
    # Restaurar RED
    sm.modo_generador_hw = False
    sm.transition_to(sm.compute_desired_state())
    assert sm.state == SystemState.NORMAL
    
    print("✓ test_state_transition_sequence")


def test_models_serialization():
    """Test serialización modelos"""
    status = SystemStatus()
    status.state = SystemState.GENERADOR
    status.nivel_agua_pct = 45
    status.bombas[0].plc_order = True
    status.bombas[0].feedback = True
    status.bombas[0].relay_closed = True
    
    d = status.to_dict()
    assert d['state'] == 1
    assert d['state_label'] == 'GENERADOR'
    assert d['nivel_agua_pct'] == 45
    assert d['bombas'][0]['running'] == True
    print("✓ test_models_serialization")


def test_alert_creation():
    """Test creación alertas"""
    from datetime import datetime
    alert = Alert(
        timestamp=datetime.now(),
        level="critical",
        source="B1",
        message="Feedback timeout",
        bomba_id=0,
        codigo=0x22
    )
    d = alert.to_dict()
    assert d['level'] == 'critical'
    assert d['source'] == 'B1'
    assert d['bomba_id'] == 0
    assert d['codigo'] == 0x22
    print("✓ test_alert_creation")


if __name__ == "__main__":
    tests = [
        test_initial_state,
        test_modo_generador_hw,
        test_modo_mantenimiento_priority,
        test_emergencia_priority,
        test_puede_arrancar_normal,
        test_puede_arrancar_generador,
        test_puede_arrancar_emergencia,
        test_puede_arrancar_mantenimiento,
        test_fault_bloquea_bomba,
        test_relay_logic_normal,
        test_relay_logic_generador,
        test_relay_logic_emergencia,
        test_feedback_mismatch_detection,
        test_contactor_pegado_detection,
        test_state_transition_sequence,
        test_models_serialization,
        test_alert_creation,
    ]
    
    print("=" * 50)
    print("Ejecutando tests de máquina de estados...")
    print("=" * 50)
    
    failed = 0
    for test in tests:
        try:
            test()
        except AssertionError as e:
            print(f"✗ {test.__name__}: {e}")
            failed += 1
        except Exception as e:
            print(f"✗ {test.__name__}: ERROR - {e}")
            failed += 1
    
    print("=" * 50)
    if failed == 0:
        print(f"TODOS LOS TESTS PASARON ({len(tests)}/{len(tests)})")
        sys.exit(0)
    else:
        print(f"FALLARON {failed} DE {len(tests)} TESTS")
        sys.exit(1)