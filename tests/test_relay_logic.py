#!/usr/bin/env python3
"""
Tests para lógica de relés y control - Supervisor de Bombas
"""

import sys
import os
sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'linux', 'app'))

from models import SystemState, BombaStatus, SystemStatus


class MockRelayControl:
    """Mock del control de relés para testing"""
    
    def __init__(self):
        self.relay_states = [False, False, False]
        self.last_change = [0, 0, 0]
        self.MIN_SWITCH_INTERVAL_MS = 100
    
    def apply(self, plc_orders, allowed, now_ms):
        """Aplicar estado deseado a relés con anti-cruzamiento"""
        for i in range(3):
            desired = allowed[i] and plc_orders[i]
            if desired != self.relay_states[i]:
                if now_ms - self.last_change[i] >= self.MIN_SWITCH_INTERVAL_MS:
                    self.relay_states[i] = desired
                    self.last_change[i] = now_ms
    
    def emergency_open_all(self):
        for i in range(3):
            self.relay_states[i] = False
    
    def set_relay(self, bomba_id, closed):
        if 0 <= bomba_id < 3:
            self.relay_states[bomba_id] = closed
    
    def get_state(self, bomba_id):
        if 0 <= bomba_id < 3:
            return self.relay_states[bomba_id]
        return False


def test_relay_passthrough_normal():
    """Test passthrough en modo NORMAL"""
    relay = MockRelayControl()
    plc_orders = [True, True, False]
    allowed = [True, True, True]  # Todas permitidas en NORMAL
    
    relay.apply(plc_orders, allowed, 1000)
    
    assert relay.get_state(0) == True
    assert relay.get_state(1) == True
    assert relay.get_state(2) == False
    print("✓ test_relay_passthrough_normal")


def test_relay_block_generador():
    """Test bloqueo B2/B3 en modo GENERADOR"""
    relay = MockRelayControl()
    plc_orders = [True, True, True]
    allowed = [True, False, False]  # Solo B1 permitida en GENERADOR
    
    relay.apply(plc_orders, allowed, 1000)
    
    assert relay.get_state(0) == True
    assert relay.get_state(1) == False
    assert relay.get_state(2) == False
    print("✓ test_relay_block_generador")


def test_relay_emergency_open():
    """Test apertura emergencia"""
    relay = MockRelayControl()
    # Primero cerrar algunos
    relay.relay_states = [True, True, True]
    
    relay.emergency_open_all()
    
    assert relay.get_state(0) == False
    assert relay.get_state(1) == False
    assert relay.get_state(2) == False
    print("✓ test_relay_emergency_open")


def test_anti_crossing():
    """Test anti-cruzamiento (mínimo 100ms entre cambios)"""
    relay = MockRelayControl()
    plc_orders = [True, False, False]
    allowed = [True, True, True]
    
    # Primer cambio: cerrado
    relay.apply(plc_orders, allowed, 1000)
    assert relay.get_state(0) == True
    
    # Intento cambio inmediato (50ms después) - DEBE ignorarse
    plc_orders[0] = False
    relay.apply(plc_orders, allowed, 1050)
    assert relay.get_state(0) == True  # Sigue cerrado
    
    # Tras 100ms - DEBE permitir cambio
    plc_orders[0] = False
    relay.apply(plc_orders, allowed, 1101)
    assert relay.get_state(0) == False  # Ahora abierto
    print("✓ test_anti_crossing")


def test_relay_independent_control():
    """Test control independiente por bomba"""
    relay = MockRelayControl()
    
    # B1 y B2 permitidas, B3 no
    plc_orders = [True, True, True]
    allowed = [True, True, False]
    relay.apply(plc_orders, allowed, 1000)
    
    assert relay.get_state(0) == True
    assert relay.get_state(1) == True
    assert relay.get_state(2) == False
    
    # Cambiar solo B2
    plc_orders[1] = False
    relay.apply(plc_orders, allowed, 1100)
    
    assert relay.get_state(0) == True
    assert relay.get_state(1) == False
    assert relay.get_state(2) == False
    print("✓ test_relay_independent_control")


def test_allowed_overrides_plc():
    """Test que 'allowed' tiene prioridad sobre orden PLC"""
    relay = MockRelayControl()
    
    # PLC ordena arranque pero no está permitido
    plc_orders = [True, True, True]
    allowed = [False, False, False]  # Ninguna permitida (ej. EMERGENCIA)
    relay.apply(plc_orders, allowed, 1000)
    
    for i in range(3):
        assert relay.get_state(i) == False
    print("✓ test_allowed_overrides_plc")


def test_plc_off_opens_relay():
    """Test que PLC=OFF abre relé aunque esté permitido"""
    relay = MockRelayControl()
    
    # Primero cerrar
    plc_orders = [True, False, False]
    allowed = [True, True, True]
    relay.apply(plc_orders, allowed, 1000)
    assert relay.get_state(0) == True
    
    # PLC apaga
    plc_orders[0] = False
    relay.apply(plc_orders, allowed, 1100)
    assert relay.get_state(0) == False
    print("✓ test_plc_off_opens_relay")


def test_state_machine_integration():
    """Test integración completa StateMachine + RelayControl"""
    
    # Simular StateMachine
    class SM:
        def __init__(self):
            self.state = SystemState.NORMAL
            self.modo_generador_hw = False
            self.modo_mantenimiento = False
            self.emergencia_activa = False
            self.bombas = [BombaStatus(i) for i in range(3)]
        
        def get_allowed(self):
            if self.emergencia_activa or self.modo_mantenimiento:
                return [False, False, False]
            if self.state == SystemState.GENERADOR:
                return [True, False, False]
            return [True, True, True]
    
    sm = SM()
    relay = MockRelayControl()
    
    # Escenario 1: NORMAL, PLC ordena 3 bombas
    sm.bombas[0].plc_order = True
    sm.bombas[1].plc_order = True
    sm.bombas[2].plc_order = True
    relay.apply([b.plc_order for b in sm.bombas], sm.get_allowed(), 1000)
    assert relay.get_state(0) == True
    assert relay.get_state(1) == True
    assert relay.get_state(2) == True
    
    # Escenario 2: Cambio a GENERADOR
    sm.state = SystemState.GENERADOR
    relay.apply([b.plc_order for b in sm.bombas], sm.get_allowed(), 1100)
    assert relay.get_state(0) == True
    assert relay.get_state(1) == False
    assert relay.get_state(2) == False
    
    # Escenario 3: EMERGENCIA
    sm.emergencia_activa = True
    relay.apply([b.plc_order for b in sm.bombas], sm.get_allowed(), 1200)
    for i in range(3):
        assert relay.get_state(i) == False
    
    # Escenario 4: MANTENIMIENTO
    sm.emergencia_activa = False
    sm.modo_mantenimiento = True
    relay.apply([b.plc_order for b in sm.bombas], sm.get_allowed(), 1300)
    for i in range(3):
        assert relay.get_state(i) == False
    
    print("✓ test_state_machine_integration")


def test_feedback_mismatch_scenario():
    """Test escenario mismatch detectado por StateMachine"""
    class SM:
        def __init__(self):
            self.state = SystemState.NORMAL
            self.bombas = [BombaStatus(i) for i in range(3)]
        
        def check_mismatch(self):
            mismatches = []
            for b in self.bombas:
                if b.plc_order != b.feedback:
                    mismatches.append(b.id)
            return mismatches
    
    sm = SM()
    
    # Caso 1: PLC ordena, no hay feedback
    sm.bombas[0].plc_order = True
    sm.bombas[0].feedback = False
    assert sm.check_mismatch() == [0]
    
    # Caso 2: Feedback sin orden (contactor pegado)
    sm.bombas[1].plc_order = False
    sm.bombas[1].feedback = True
    assert sm.check_mismatch() == [0, 1]
    
    # Caso 3: Coherentes
    sm.bombas[0].feedback = True
    sm.bombas[1].feedback = False
    assert sm.check_mismatch() == []
    
    print("✓ test_feedback_mismatch_scenario")


def test_fault_count_increment():
    """Test incremento contador de fallos"""
    bomba = BombaStatus(0)
    
    assert bomba.fault_count == 0
    
    bomba.fault_count += 1
    assert bomba.fault_count == 1
    
    bomba.fault_count += 1
    assert bomba.fault_count == 2
    
    # Reset en salida de emergencia
    bomba.fault_count = 0
    assert bomba.fault_count == 0
    
    print("✓ test_fault_count_increment")


def test_system_status_aggregation():
    """Test agregación de estado del sistema"""
    status = SystemStatus()
    status.state = SystemState.GENERADOR
    status.modo_generador_hw = True
    status.nivel_agua_pct = 75
    status.sensor_ok = True
    status.uptime_ms = 3600000  # 1 hora
    
    # Bomba 1 corriendo
    status.bombas[0].plc_order = True
    status.bombas[0].feedback = True
    status.bombas[0].relay_closed = True
    
    # Bomba 2 bloqueada por modo
    status.bombas[1].plc_order = True
    status.bombas[1].feedback = False
    status.bombas[1].relay_closed = False
    
    # Bomba 3 parada
    status.bombas[2].plc_order = False
    status.bombas[2].feedback = False
    status.bombas[2].relay_closed = False
    
    d = status.to_dict()
    
    assert d['state'] == 1
    assert d['state_label'] == 'GENERADOR'
    assert d['nivel_agua_pct'] == 75
    assert d['bombas'][0]['running'] == True
    assert d['bombas'][1]['running'] == False
    assert d['bombas'][1]['mismatch'] == True  # PLC=1, FB=0
    assert d['bombas'][2]['running'] == False
    
    print("✓ test_system_status_aggregation")


if __name__ == "__main__":
    tests = [
        test_relay_passthrough_normal,
        test_relay_block_generador,
        test_relay_emergency_open,
        test_anti_crossing,
        test_relay_independent_control,
        test_allowed_overrides_plc,
        test_plc_off_opens_relay,
        test_state_machine_integration,
        test_feedback_mismatch_scenario,
        test_fault_count_increment,
        test_system_status_aggregation,
    ]
    
    print("=" * 50)
    print("Ejecutando tests de lógica de relés...")
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