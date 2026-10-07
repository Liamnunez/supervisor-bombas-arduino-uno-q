#!/usr/bin/env python3
"""
Tests de las correcciones de la auditoria docs<->codigo.

Cada test cubre un fallo concreto que la auditoria encontro. La idea es que
si alguien revierte una correccion, el fallo sale aqui y no tres meses
despues en planta.

Contexto de los dos mas importantes:

- La alerta de "sin heartbeat" vivia en check_thresholds(), que solo se
  llama desde on_status_change(), que a su vez solo dispara al decodificar
  un frame del MCU. Cortar el UART -> no llegan frames -> no se evalua
  ningun umbral -> la alerta no suena. La ausencia de datos era
  indistinguible de la ausencia de problemas.
- La retencion de docs/severity_levels.md (30/90 dias, 5 anos para
  seguridad) no existia: no habia ni un DELETE en el proyecto.
"""

import datetime
import os
import sys
import tempfile

sys.path.insert(0, os.path.join(os.path.dirname(__file__), '..', 'linux'))

from app.alerts import AlertManager, Alert, AlertStore
from app.state import MCU_ERROR_LEVELS, MCU_ERROR_MESSAGES, _crc8


def _manager():
    return AlertManager(db_path=os.path.join(tempfile.mkdtemp(), "t.db"))


# ---------------------------------------------------------------- silencio
def test_silence_no_avisa_dentro_del_umbral():
    man = _manager()
    assert man.check_silence(man.heartbeat_timeout_s) == []
    assert man.check_silence(0.0) == []


def test_silence_avisa_pasado_el_umbral():
    """Es la alerta que tiene que sonar cuando se corta el UART."""
    man = _manager()
    alertas = man.check_silence(man.heartbeat_timeout_s + 5)
    assert len(alertas) == 1
    a = alertas[0]
    assert a.level == "critical"
    assert a.source == "MCU"
    assert "heartbeat" in a.message.lower()


def test_silence_no_depende_del_estado_del_mcu():
    """La clave del arreglo: silence() no recibe SystemStatus.

    Si dependiera del estado, no podria evaluarse cuando no hay datos, que
    es exactamente el caso que hay que detectar.
    """
    import inspect
    params = list(inspect.signature(AlertManager.check_silence).parameters)
    assert params == ["self", "segundos"], params


def test_silence_es_la_unica_via_de_la_alerta_de_mcu():
    """check_thresholds ya no debe duplicar la alerta de silencio.

    Duplicarla con otro texto hacia que el dashboard y Telegram pudieran
    mostrar dos mensajes distintos para el mismo fallo.
    """
    import inspect
    src = inspect.getsource(AlertManager.check_thresholds)
    assert "Sin heartbeat" not in src, "check_thresholds sigue duplicando la alerta"


# --------------------------------------------------------------- retencion
def test_purge_respeta_las_retenciones():
    man = _manager()
    ahora = datetime.datetime.now()
    # Viejas por debajo de su retencion -> se borran
    for nivel, dias in (("info", 400), ("warning", 400)):
        man.add(Alert(timestamp=ahora - datetime.timedelta(days=dias),
                      level=nivel, source="T", message="vieja"))
    # Recientes -> se conservan
    for nivel in ("info", "warning", "critical"):
        man.add(Alert(timestamp=ahora, level=nivel, source="T", message="reciente"))

    borradas = man.purge()
    assert borradas == 2, f"esperaba 2, borradas {borradas}"
    quedan = {a.message for a in man.store.recent(50)}
    assert quedan == {"reciente"}


def test_purge_conserva_critical_hasta_5_anos():
    """Los registros de seguridad se guardan 5 años, no 90 días.

    Un critical de hace 400 días tiene que sobrevivir a la purga aunque un
    warning de la misma antigüedad no sobreviva.
    """
    man = _manager()
    ahora = datetime.datetime.now()
    man.add(Alert(timestamp=ahora - datetime.timedelta(days=400),
                  level="critical", source="T", message="critico viejo"))
    man.add(Alert(timestamp=ahora - datetime.timedelta(days=400),
                  level="warning", source="T", message="warning viejo"))
    man.add(Alert(timestamp=ahora, level="critical", source="T", message="critico nuevo"))
    man.purge()
    quedan = {a.message for a in man.store.recent(50)}
    assert quedan == {"critico viejo", "critico nuevo"}, quedan


def test_purge_borra_critical_mas_all_de_5_anos():
    """Aun los registros de seguridad tienen un límite."""
    man = _manager()
    man.add(Alert(timestamp=datetime.datetime.now() - datetime.timedelta(days=2000),
                  level="critical", source="T", message="critico prehistorico"))
    man.purge()
    assert man.store.recent(50) == []


def test_purge_es_idempotente():
    man = _manager()
    ahora = datetime.datetime.now()
    man.add(Alert(timestamp=ahora - datetime.timedelta(days=400),
                  level="info", source="T", message="vieja"))
    assert man.purge() == 1
    assert man.purge() == 0


def test_purge_sin_store_no_revienta():
    """Sin base de datos no debe lanzar: el arranque sin MCU es normal."""
    man = AlertManager()
    assert man.purge() == 0


# --------------------------------------------------------- request_status
def test_request_status_manda_heartbeat():
    """Era un no-op; ahora pide estado de verdad."""
    class FakeSerial:
        is_open = True

        def __init__(self):
            self.escrito = []

        def write(self, data):
            self.escrito.append(bytes(data))
            return len(data)

        def flush(self):
            pass

    from app.state import StateManager
    sm = StateManager()
    sm.serial = FakeSerial()
    assert sm.request_status() is True
    assert len(sm.serial.escrito) == 1
    frame = sm.serial.escrito[0]
    assert frame[0] == 0xAA
    assert frame[1] == 0x50, f"esperaba HEARTBEAT, vino {frame[1]:#04x}"
    assert frame[10] == 0x55
    # y el CRC tiene que ser el del protocolo, no uno cualquiera
    assert _crc8(frame[1:9]) == frame[9]


def test_request_status_sin_puerto_no_lanza():
    from app.state import StateManager
    sm = StateManager()
    sm.serial = None
    assert sm.request_status() is False


# ------------------------------------------------------ codigos de error
def test_nivel_sensor_tiene_codigo_y_texto():
    """SIF-05 ahora emite 0x6004, antes solo detectaba sin avisar."""
    assert 0x6004 in MCU_ERROR_MESSAGES
    assert "nivel" in MCU_ERROR_MESSAGES[0x6004].lower()
    # y por defecto es critical: una lectura no confiable es relevante
    assert MCU_ERROR_LEVELS.get(0x6004, "critical") == "critical"


def test_trip_no_es_critical():
    """0x6001 cae en warning: cada trip normal en rojo = fatiga de alertas."""
    assert MCU_ERROR_LEVELS[0x6001] == "warning"
    assert MCU_ERROR_LEVELS[0x6020] == "info"


if __name__ == '__main__':
    tests = [v for k, v in sorted(globals().items()) if k.startswith('test_')]
    fallos = 0
    for t in tests:
        try:
            t()
            print(f"OK  {t.__name__}")
        except Exception as e:
            print(f"FAIL {t.__name__}: {e}")
            fallos += 1
    print(f"\n{len(tests)} tests, {fallos} fallos")
    sys.exit(1 if fallos else 0)
