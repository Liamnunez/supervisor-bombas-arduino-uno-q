"""
Verificación adversarial del protocolo de 11 bytes en linux/app/state.py.

El frame está duplicado en tres sitios (AGENTS.md "Traps"):
  - mcu/include/config.h     -> McuMessage, crc8(), validar_mensaje()
  - mcu/src/comm_bridge.cpp  -> buildMessage()
  - linux/app/state.py       -> _validate_crc(), send_command()

Este test ejecuta el código REAL de app.state (no una reimplementación) y
comprueba la coherencia de las tres copias:

  1. El CRC8 de Python (state.py) coincide con el de C++ (config.h).
  2. `_validate_crc()` acepta los frames bien formados y rechaza la basura.
  3. `send_command()` produce frames que el firmware aceptaría.
  4. El parser se resincroniza tras ruido y conserva lo incompleto.
  5. Los contadores de diagnóstico cuentan lo que dicen contar.

Tabla de CRCs (sacada compilando y ejecutando config.h:185, no de memoria):
  crc8("", 0)            = 0x00
  crc8({0x00})           = 0x00
  crc8({0xff})           = 0xF3
  crc8({0xaa})           = 0x5F
  crc8({0x55})           = 0xAC
  crc8({01 02 03 04 05}) = 0xBC
  crc8("123456789")      = 0xF4
"""

import struct
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "linux"))

from app.state import StateManager  # noqa: E402

MSG_SIZE = 11


# ============================================================
# CRC8 de referencia (Dallas/Maxim, poly 0x07), enmascarado a 8 bits
# como lo hace el uint8_t de C en config.h.
# ============================================================
def crc8_ref(data: bytes) -> int:
    """Equivalente exacto de crc8() en mcu/include/config.h:185-194."""
    crc = 0
    for b in data:
        crc ^= b
        for _ in range(8):
            crc = ((crc << 1) ^ 0x07) & 0xFF if (crc & 0x80) else (crc << 1) & 0xFF
    return crc


def cuerpo(msg_type=0x50, bomba_id=0xFF, ts=0, payload=0) -> bytes:
    """Los 9 primeros bytes del frame: 0xAA|type|id|ts(4)|payload(2)."""
    b = bytearray(9)
    b[0] = 0xAA
    b[1] = msg_type
    b[2] = bomba_id
    b[3:7] = struct.pack("<I", ts)
    b[7:9] = struct.pack("<H", payload)
    return bytes(b)


def frame_que_acepta_el_firmware(**kw) -> bytes:
    """Frame bien formado segun el contrato CORREGIDO del protocolo.

    Contrato unico, el mismo en los tres sitios que lo implementan:
        validar_mensaje()  mcu/include/config.h
        buildMessage()    mcu/src/comm_bridge.cpp
        _validate_crc()   linux/app/state.py

    CRC8 sobre los offsets [1..8]: msg_type, bomba_id, timestamp(4) y
    payload(2). Quedan fuera start_byte (0), crc8 (9) y end_byte (10).

    Antes de corregirse, las tres copias usaban rangos distintos (0..8,
    1..8 y 1..9) y el firmware no aceptaba sus propios frames. Este helper
    construía el frame segun el rango viejo; por eso fallaba tras el
    arreglo. test_rangos_del_crc_coinciden_en_las_tres_copias lo ata ahora.
    """
    c = cuerpo(**kw)
    return c + bytes([crc8_ref(c[1:9]), 0x55])


class FakeSerial:
    """Puerto serie de mentira que captura lo que se escribe."""

    def __init__(self):
        self.is_open = True
        self.escrito = []

    def write(self, data):
        self.escrito.append(bytes(data))
        return len(data)

    def flush(self):
        pass


@pytest.fixture
def sm():
    m = StateManager()
    m.serial = FakeSerial()
    return m


# ============================================================
# 1. El CRC8 de Python == el de C++
# ============================================================
def test_crc8_python_coincide_con_cpp():
    assert crc8_ref(b"") == 0x00
    assert crc8_ref(b"\x00") == 0x00
    assert crc8_ref(b"\xff") == 0xF3
    assert crc8_ref(b"\xaa") == 0x5F
    assert crc8_ref(b"\x55") == 0xAC
    assert crc8_ref(b"\x01\x02\x03\x04\x05") == 0xBC
    assert crc8_ref(b"123456789") == 0xF4      # check vector CRC-8/SMBUS


def test_crc8_detecta_cualquier_flip_de_un_bit():
    base = bytes([0xAA, 0x50, 0xFF, 0x00, 0x00, 0x00, 0x01, 0x00, 0x2A])
    ref = crc8_ref(base)
    for i in range(len(base)):
        for bit in range(8):
            alterado = bytearray(base)
            alterado[i] ^= 1 << bit
            assert crc8_ref(bytes(alterado)) != ref, (i, bit)


def test_crc8_de_state_py_no_esta_enmascarado():
    """
    Hallazgo: el CRC8 de state.py:170-174 NO está enmascarado a 8 bits.

    En C, `uint8_t crc` se trunca solo en cada iteración. En Python los
    enteros no se truncan, así que `crc = (crc << 1) ^ 0x07` crece sin
    límite: tras 9 bytes vale ~2^80. Nunca puede ser igual a data[10] (0x55).

    Se comprueba con el código de state.py copiado literalmente.
    """
    f = frame_que_acepta_el_firmware()
    crc = 0
    for b in f[1:10]:
        crc ^= b
        for _ in range(8):
            crc = (crc << 1) ^ 0x07 if (crc & 0x80) else (crc << 1)
    assert crc > 0xFF, "el CRC de state.py debería desbordar 8 bits"
    assert crc != f[10]
    # Y el valor enmascarado sí es el correcto: la falta de máscara es el
    # único defecto del algoritmo en sí.
    assert crc & 0xFF == crc8_ref(f[1:10])


# ============================================================
# 2. _validate_crc: acepta lo bueno, rechaza la basura
# ============================================================
def test_validate_crc_acepta_frame_bien_formado(sm):
    f = frame_que_acepta_el_firmware()
    assert len(f) == MSG_SIZE
    assert sm._validate_crc(f) is True


def test_validate_crc_acepta_todos_los_opcodes(sm):
    for t in (0x10, 0x20, 0x21, 0x22, 0x23, 0x30, 0x31, 0x40, 0x50, 0xFF):
        f = frame_que_acepta_el_firmware(msg_type=t, ts=12345, payload=0xBEEF)
        assert sm._validate_crc(f) is True, hex(t)


def test_validate_crc_con_1000_frames_aleatorios(sm):
    import random

    rnd = random.Random(1234)          # semilla fija: determinista
    aceptados = 0
    for _ in range(1000):
        f = frame_que_acepta_el_firmware(
            msg_type=rnd.randrange(256),
            bomba_id=rnd.randrange(256),
            ts=rnd.randrange(2**32),
            payload=rnd.randrange(2**16),
        )
        if sm._validate_crc(f):
            aceptados += 1
    assert aceptados == 1000, f"solo {aceptados}/1000 aceptados"


def test_validate_crc_rechaza_crc_malo(sm):
    f = bytearray(frame_que_acepta_el_firmware())
    f[9] ^= 0xFF
    assert sm._validate_crc(bytes(f)) is False


def test_crc_no_cubre_los_bytes_de_encuadre(sm):
    """El CRC firma [1..8]: start_byte, crc8 y end_byte quedan fuera.

    Por eso un start_byte corrupto NO cambia el CRC. No es un agujero: es
    la razón de que los bytes de encuadre se comprueben explícitamente, en
    el firmware con msg.start_byte == 0xAA y aquí con el check de
    _handle_message(). Estos tests existían exigiendo que el CRC cubriera
    el encuadre, que era el contrato viejo (rango 0..8) y la razón de que
    el firmware no aceptara sus propios frames.
    """
    bueno = frame_que_acepta_el_firmware()
    f = bytearray(bueno)
    f[0] = 0xAB
    assert sm._validate_crc(bytes(f)) is True   # el CRC no lo ve
    f[10] = 0x56
    assert sm._validate_crc(bytes(f)) is True   # tampoco el end byte


def test_parser_rechaza_start_byte_malo(sm):
    """Quien tiene que rechazar el start byte es el parser, no el CRC."""
    f = bytearray(frame_que_acepta_el_firmware())
    f[0] = 0xAB
    antes = sm.rx_count
    sm._handle_message(bytes(f))
    assert sm.rx_count == antes, "el parser aceptó un frame sin 0xAA"


def test_parser_rechaza_end_byte_malo(sm):
    f = bytearray(frame_que_acepta_el_firmware())
    f[10] = 0x56
    antes = sm.rx_count
    sm._handle_message(bytes(f))
    assert sm.rx_count == antes, "el parser aceptó un frame sin 0x55"


def test_validate_crc_rechaza_flip_de_un_bit_en_el_payload(sm):
    f = bytearray(frame_que_acepta_el_firmware(payload=0x1234))
    f[7] ^= 0x01
    assert sm._validate_crc(bytes(f)) is False


def test_validate_crc_revienta_con_data_corta(sm):
    """
    Fragilidad de la API interna: state.py:174 compara con data[10], así que
    con menos de 11 bytes lanza IndexError en vez de devolver False.

    NO es alcanzable desde el parser: _process_buffer() (state.py:117-142)
    sólo extrae bloques de exactamente MSG_SIZE bytes antes de llamar a
    _handle_message(). Se documenta como fragilidad, no como fallo del flujo.
    """
    with pytest.raises(IndexError):
        sm._validate_crc(frame_que_acepta_el_firmware()[:8])


# ============================================================
# 3. send_command
# ============================================================
def test_send_command_escribe_frame_aceptado_por_el_firmware(sm):
    ok = sm.set_modo_generador(True)
    assert ok is True, f"send_command devolvió {ok}"
    assert len(sm.serial.escrito) == 1, "no se escribió nada en el UART"
    f = sm.serial.escrito[0]
    assert len(f) == MSG_SIZE
    assert f[0] == 0xAA
    assert f[10] == 0x55
    # Contrato único [1..8], el mismo en config.h, comm_bridge.cpp y aquí
    assert f[9] == crc8_ref(f[1:9]), (
        "el CRC que envía state.py no es el que valida el firmware"
    )


@pytest.mark.parametrize(
    "llamada,esperado_type,esperado_payload",
    [
        (lambda s: s.set_modo_generador(True), 0x40, 1),
        (lambda s: s.set_modo_generador(False), 0x40, 0),
        (lambda s: s.set_mantenimiento(True), 0x10, 3),
        (lambda s: s.reset_emergencia(), 0xFF, 0xFFFF),
    ],
)
def test_send_command_opcodes(sm, llamada, esperado_type, esperado_payload):
    assert llamada(sm) is True
    f = sm.serial.escrito[0]
    assert f[1] == esperado_type, hex(f[1])
    assert struct.unpack("<H", f[7:9])[0] == esperado_payload


def test_send_command_trigger_emergencia(sm):
    assert sm.trigger_emergencia(0xFF00) is True
    f = sm.serial.escrito[0]
    # state.py:310 -> payload = ((codigo & 0xFF) << 8) | 2
    assert struct.unpack("<H", f[7:9])[0] == ((0xFF00 & 0xFF) << 8) | 2


def test_request_status_no_crea_frame(sm):
    # state.py:316-319 -> request_status es un no-op explícito
    assert sm.request_status() is True
    assert sm.serial.escrito == []


def test_send_command_sin_puerto_devuelve_false():
    m = StateManager()
    m.serial = None
    assert m.set_modo_generador(True) is False
    assert m.reset_emergencia() is False
    assert m.set_mantenimiento(True) is False


def test_send_command_con_puerto_cerrado_devuelve_false():
    class Cerrado(FakeSerial):
        def __init__(self):
            super().__init__()
            self.is_open = False

    m = StateManager()
    m.serial = Cerrado()
    assert m.set_modo_generador(True) is False


# ============================================================
# 4. El parser
# ============================================================
def test_parser_procesa_un_frame_valido(sm):
    sm.rx_buffer = bytearray(frame_que_acepta_el_firmware())
    sm._process_buffer()
    assert len(sm.rx_buffer) == 0
    assert sm.rx_count == 1
    assert sm.crc_errors == 0


def test_parser_descarta_basura_y_encuentra_el_frame(sm):
    sm.rx_buffer = bytearray(b"\x00\x01\xff\x13\x37" + frame_que_acepta_el_firmware())
    sm._process_buffer()
    assert len(sm.rx_buffer) == 0
    assert sm.rx_count == 1


def test_parser_conserva_el_frame_incompleto(sm):
    f = frame_que_acepta_el_firmware()
    sm.rx_buffer = bytearray(f[:6])
    sm._process_buffer()
    assert len(sm.rx_buffer) == 6, "un frame a medias debe esperar"


def test_parser_limpia_buffer_sin_start_byte(sm):
    sm.rx_buffer = bytearray(b"\x01\x02\x03\x04\x05\x06\x07\x08\x09\x0a\x0b")
    sm._process_buffer()
    assert len(sm.rx_buffer) == 0


def test_dos_frames_seguidos(sm):
    sm.rx_buffer = bytearray(
        frame_que_acepta_el_firmware() + frame_que_acepta_el_firmware()
    )
    sm._process_buffer()
    assert len(sm.rx_buffer) == 0
    assert sm.rx_count == 2


def test_contador_crc_errors_sube_con_frame_corrupto(sm):
    malo = bytearray(frame_que_acepta_el_firmware())
    malo[9] ^= 0xFF
    sm.rx_buffer = bytearray(bytes(malo))
    sm._process_buffer()
    assert sm.rx_count == 0, "un frame con CRC malo no debe contar como recibido"
    assert sm.crc_errors == 1


def test_get_stats_refleja_los_contadores(sm):
    sm.rx_buffer = bytearray(frame_que_acepta_el_firmware())
    sm._process_buffer()
    s = sm.get_stats()
    assert s["rx_count"] == 1
    assert s["crc_errors"] == 0
    assert s["connected"] is True