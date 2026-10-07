#!/usr/bin/env python3
"""
Tests de `tools/sign_firmware.py` (Fase 4, paso 1).

Qué se prueba aquí es la **lógica de rechazo**, que es la parte de seguridad.
La criptografía real no se reimplementa: se usa una libreria auditada, igual
que el firmware hara con monocypher. Si el hash o la firma estuvieran mal, lo
detectaria el backend, no estos tests.

Estos tests son el espejo Python de `tests/native/test_fw_image.cpp`: mismos
casos, mismo texto de rechazo, mismo orden. Los dos deben seguir dando el
mismo veredicto para la misma imagen, o el CI firmaria algo que el MCU no
acepta.

Si no hay `cryptography` ni `PyNaCl`, TODOS los tests se saltan: es mejor un
`skip` honesto que un `make tests` roto en una maquina sin dependencias. En
esa situacion la herramienta tampoco firma ni verifica (fail-closed).
"""

import importlib.util
import json
import os
import shutil
import subprocess
import sys
import zlib

import pytest

REPO_ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
TOOL_PATH = os.path.join(REPO_ROOT, "tools", "sign_firmware.py")

_spec = importlib.util.spec_from_file_location("sign_firmware", TOOL_PATH)
sf = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(sf)

# Sin backend criptografico no se puede firmar nada: se salta todo el fichero.
pytestmark = pytest.mark.skipif(
    not sf.HAVE_CRYPTO,
    reason="sin 'cryptography' ni 'pynacl': se necesita Ed25519 real "
           "(pip install cryptography)")

# Mismos vectores que usa el test nativo, para que si divergen se note.
SLOT_MAX = sf.DEFAULT_SLOT_MAX
VERSION = 12
BUILD_ID = 0x1234

# Programa auxiliar: pide a `mcu/src/fw_image.cpp` un manifiesto y lo escribe
# en disco. Sirve para comparar byte a byte el formato de los dos extremos.
# El SHA-256 lo pone a cero porque el C++ todavia no tiene CryptoBackend real;
# el resto de campos si los produce el firmware de verdad.
CPP_MANIFEST_PROGRAM = r"""
#include "fw_image.h"
#include <cstdio>
#include <cstring>
#include <fstream>
#include <vector>

// Escribe un manifiesto con el MISMO codigo que usa el firmware de verdad:
// fwCrc32() para el CRC del payload y para el CRC del manifiesto.
int main() {
    std::vector<uint8_t> payload(1000);
    for (size_t i = 0; i < payload.size(); i++)
        payload[i] = static_cast<uint8_t>(i * 13 + 1);

    std::vector<uint8_t> img(FW_HEADER_LEN, 0);
    uint8_t sha_cero[32] = {0};   // el C++ aun no tiene SHA-256 real

    struct Campo { size_t off; uint32_t valor; } campos[] = {
        {0x00, FW_MAGIC},
        {0x04, FW_HEADER_LEN},
        {0x08, 7u},
        {0x0C, static_cast<uint32_t>(payload.size())},
        {0x10, 0xABCDu},
        {0x14, fwCrc32(payload.data(), payload.size())},
    };
    for (const Campo& c : campos) {
        img[c.off + 0] = static_cast<uint8_t>(c.valor);
        img[c.off + 1] = static_cast<uint8_t>(c.valor >> 8);
        img[c.off + 2] = static_cast<uint8_t>(c.valor >> 16);
        img[c.off + 3] = static_cast<uint8_t>(c.valor >> 24);
    }
    std::memcpy(img.data() + 0x18, sha_cero, 32);

    // reserved (0x3C) = 0, ya esta a cero en el vector.
    // header_crc32 (0x38) = fwCrc32 de los bytes [0x00, 0x38).
    const uint32_t h = fwCrc32(img.data(), 0x38);
    img[0x38] = static_cast<uint8_t>(h);
    img[0x39] = static_cast<uint8_t>(h >> 8);
    img[0x3A] = static_cast<uint8_t>(h >> 16);
    img[0x3B] = static_cast<uint8_t>(h >> 24);

    std::ofstream f("@OUT@", std::ios::binary);
    f.write(reinterpret_cast<const char*>(img.data()),
            static_cast<std::streamsize>(img.size()));
    f.close();
    std::printf("%u\n", FW_HEADER_LEN);
    return 0;
}
"""


# --- Utilidades de prueba ----------------------------------------------------

def _payload(n=4096):
    """Payload determinista, no aleatorio: si falla, se puede reproducir."""
    return bytes((i * 7 + 3) & 0xFF for i in range(n))


def _backend():
    """Backend con la clave publica derivada de una privada efimera."""
    backend, _pub_raw, _priv, _pub = sf.generar_claves()
    return backend


def _firmar(payload=None, version=VERSION, build_id=BUILD_ID):
    """Firma un payload y devuelve la imagen completa."""
    backend = _backend()
    img = sf.build_image(payload if payload is not None else _payload(),
                         version, build_id, backend)
    return img, backend


def _verificar(img, backend, **kwargs):
    return sf.verify_image(img, crypto=backend, **kwargs)


def _manifiesto(payload, version, build_id, sha):
    """Manifiesto de 64 bytes armado con las mismas reglas que el C++.

    `sha` va aparte para poder comparar contra el manifiesto del C++, que aun
    no tiene SHA-256 real y pone ceros ahi.
    """
    hdr = bytearray(64)
    sf.put32(hdr, 0x00, sf.MAGIC)
    sf.put32(hdr, 0x04, 64)
    sf.put32(hdr, 0x08, version)
    sf.put32(hdr, 0x0C, len(payload))
    sf.put32(hdr, 0x10, build_id)
    sf.put32(hdr, 0x14, sf.crc32(payload))
    hdr[0x18:0x18 + sf.SHA256_LEN] = sha
    # El CRC del manifiesto se calcula DESPUES de escribir todo lo anterior.
    sf.put32(hdr, 0x38, sf.crc32(bytes(hdr[0:sf.HEADER_CRC_END])))
    sf.put32(hdr, 0x3C, 0)
    return bytes(hdr)


def _recalcular_header_crc(img):
    """Recalcula header_crc32 tras manipular el manifiesto (0x38).

    Es lo que haria un atacante: arreglar el CRC del manifiesto para pasar el
    filtro barato. Sirve para que los rechazos de los pasos siguientes sean los
    de verdad y no el del CRC del manifiesto.
    """
    sf.put32(img, 0x38, sf.crc32(bytes(img[0:sf.HEADER_CRC_END])))
    return img


def _mutable(img):
    return bytearray(img)


# --- Imagen valida -----------------------------------------------------------

def test_imagen_valida_aceptada():
    """Una imagen recien firmada tiene que pasar los 9 pasos."""
    img, backend = _firmar()
    v = _verificar(img, backend)

    assert v.ok(), sf.reject_text(v.reject)
    assert v.reject == sf.FwReject.NONE
    assert v.version == VERSION
    assert v.payload_len == 4096
    assert v.build_id == BUILD_ID
    assert v.total_len == sf.HEADER_LEN + 4096 + sf.SIGNATURE_LEN
    assert len(img) == v.total_len
    assert v.steps_done == 9


def test_manifiesto_ocupa_64_bytes():
    """header_len del manifiesto es fijo: si cambia, el MCU no lo acepta."""
    img, _ = _firmar()
    m = sf.parse_manifest(img)
    assert m.header_len == 64 == sf.HEADER_LEN
    assert m.magic == sf.MAGIC
    assert m.reserved == 0
    assert img[64:64 + 4096] == _payload()


def test_header_crc32_cubre_solo_lo_anterior():
    """El CRC del manifiesto se calcula sobre [0x00, 0x38), 56 bytes."""
    img, _ = _firmar()
    m = sf.parse_manifest(img)
    assert sf.crc32(img[0:sf.HEADER_CRC_END]) == m.header_crc32
    # Y NO cubre el campo reserved de 0x3C: cambiarlo no rompe el CRC.
    img2 = _mutable(img)
    sf.put32(img2, 0x3C, 0xDEADBEEF)
    assert sf.crc32(img2[0:sf.HEADER_CRC_END]) == m.header_crc32


def test_manifiesto_coincide_con_el_que_escribe_el_cpp(tmp_path):
    """El manifiesto de Python tiene que ser el del C++, byte a byte.

    Se compila `mcu/src/fw_image.cpp` con g++ y se le pide un manifiesto; el
    resultado se compara con el que produce Python. Si difieren en un byte, el
    CI firmaria algo que el MCU no acepta. Es la prueba que ata las dos
    implementaciones de verdad, y la que no se puede simular con mocks.
    """
    gpp = shutil.which("g++")
    if gpp is None:
        pytest.skip("g++ no disponible: no se puede compilar fw_image.cpp")

    cpp = os.path.join(REPO_ROOT, "mcu", "src", "fw_image.cpp")
    hdr = os.path.join(REPO_ROOT, "mcu", "include", "fw_image.h")
    inc = os.path.join(REPO_ROOT, "mcu", "include")
    if not os.path.exists(cpp) or not os.path.exists(hdr):
        pytest.skip("no se encuentra fw_image.cpp")

    manifiesto_cpp = tmp_path / "cpp_manifest.bin"
    programa = tmp_path / "cpp_manifest.cpp"
    programa.write_text(CPP_MANIFEST_PROGRAM.replace(
        "@OUT@", str(manifiesto_cpp)), encoding="utf-8")

    exe = tmp_path / "cpp_manifest"
    subprocess.run([gpp, "-std=c++17", "-I", inc, str(programa), cpp,
                    "-o", str(exe)], check=True, capture_output=True)
    subprocess.run([str(exe)], check=True, capture_output=True)

    payload = bytes((i * 13 + 1) & 0xFF for i in range(1000))
    # El C++ no tiene SHA-256 todavia, asi que se compara con ceros en 0x18.
    esperado = _manifiesto(payload, 7, 0xABCD, b"\x00" * sf.SHA256_LEN)
    obtenido = manifiesto_cpp.read_bytes()

    assert len(obtenido) == sf.HEADER_LEN == 64
    assert obtenido == esperado, (
        "el manifiesto del C++ y el de Python difieren:\n"
        "  C++   : %s\n  Python: %s" % (obtenido.hex(), esperado.hex()))


def test_campos_manifiesto_en_offsets_correctos():
    """Los offsets son el contrato con el C++; un byte de mas rompe todo."""
    img, _ = _firmar()
    assert sf.rd32(img, 0x00) == sf.MAGIC
    assert sf.rd32(img, 0x04) == 64
    assert sf.rd32(img, 0x08) == VERSION
    assert sf.rd32(img, 0x0C) == 4096
    assert sf.rd32(img, 0x10) == BUILD_ID
    assert sf.rd32(img, 0x14) == zlib.crc32(_payload()) & 0xFFFFFFFF
    assert bytes(img[0x18:0x38]) == sf.crypto_sha256(_payload())
    assert sf.rd32(img, 0x3C) == 0


def test_firma_va_al_final_de_64_bytes():
    """La firma son los ultimos 64 bytes y va detras del payload."""
    img, _ = _firmar()
    assert len(img) == 64 + 4096 + 64
    assert len(img[-64:]) == 64


def test_crc32_es_el_del_firmware():
    """`zlib.crc32` tiene que ser el mismo CRC-32 que usa fwCrc32()."""
    assert sf.crc32(b"123456789") == 0xCBF43926      # vector estandar
    assert sf.crc32(b"") == 0x00000000
    assert sf.crc32(b"\x00\x00\x00\x00") == 0x2144DF1C


# --- Rechazos: identidad de la imagen ----------------------------------------

def test_magic_invalido():
    """No es nuestra imagen: se rechaza en el paso 2, sin tocar la firma."""
    img, backend = _firmar()
    img = _mutable(img)
    img[0] = ord("Z")
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.BAD_MAGIC
    assert sf.reject_text(v.reject) == "magic invalido"
    assert v.steps_done == 1


def test_header_len_alterado():
    """Un manifiesto de otro tamaño no se interpreta."""
    img, backend = _firmar()
    img = _mutable(img)
    sf.put32(img, 0x04, 32)
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.BAD_HEADER_LEN
    assert sf.reject_text(v.reject) == "tamano de manifiesto inesperado"
    assert v.steps_done == 1


def test_imagen_menor_que_manifiesto():
    """Menos de 64 bytes: ni siquiera se puede leer el manifiesto."""
    v = sf.verify_image(b"\x00" * 10, crypto=_backend())
    assert v.reject == sf.FwReject.TOO_SMALL
    assert sf.reject_text(v.reject) == "imagen menor que el manifiesto"


# --- Rechazos: bounds antes de la firma --------------------------------------

def test_payload_no_cabe_en_slot():
    """Slot mas pequeno que el payload: rechazo en el paso 3."""
    img, backend = _firmar(payload=_payload(1024))
    v = sf.verify_image(img, slot_max=512, crypto=backend)
    assert v.reject == sf.FwReject.PAYLOAD_TOO_BIG
    assert sf.reject_text(v.reject) == "payload no cabe en el slot"
    assert v.steps_done == 2
    # Con el slot exacto, sí cabe.
    assert sf.verify_image(img, slot_max=1024, crypto=backend).ok()


def test_payload_len_0_rechazado():
    """payload_len == 0 usa el mismo rechazo que un payload enorme."""
    img, backend = _firmar()
    img = _mutable(img)
    sf.put32(img, 0x0C, 0)
    _recalcular_header_crc(img)
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.PAYLOAD_TOO_BIG


def test_payload_len_enorme_rechazado_antes_de_leer():
    """payload_len absurdamente grande no debe provocar ninguna lectura.

    Es el ataque clasico de desbordamiento: el manifest dice que hay 4 GiB de
    payload y el payload real son 4 KiB. Tiene que rechazarse en el paso 3.
    """
    img, backend = _firmar()
    img = _mutable(img)
    sf.put32(img, 0x0C, 0xFFFFFFF0)
    _recalcular_header_crc(img)
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.PAYLOAD_TOO_BIG
    assert v.steps_done == 2


def test_imagen_truncada():
    """Faltan bytes para payload + firma: rechazo en el paso 4."""
    img, backend = _firmar()
    img = img[:-10]
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.IMAGE_TRUNCATED
    assert sf.reject_text(v.reject) == "imagen truncada"
    assert v.steps_done == 3


def test_solo_manifiesto_es_imagen_truncada():
    """64 bytes exactos: hay manifiesto pero no hay payload ni firma."""
    img, backend = _firmar()
    v = _verificar(img[:sf.HEADER_LEN], backend)
    assert v.reject == sf.FwReject.IMAGE_TRUNCATED


# --- Rechazos: anti-rollback -------------------------------------------------

def test_version_no_mas_nueva():
    """Misma version es reinstalacion, no actualizacion."""
    img, backend = _firmar()
    v = sf.verify_image(img, current_version=VERSION, crypto=backend)
    assert v.reject == sf.FwReject.VERSION_NOT_NEWER
    assert sf.reject_text(v.reject) == "version no mas nueva"
    assert v.steps_done == 4


def test_version_anterior_rechazada():
    """Downgrade: una version menor que la instalada."""
    img, backend = _firmar(version=5)
    assert sf.verify_image(img, current_version=VERSION, crypto=backend) \
        .reject == sf.FwReject.VERSION_NOT_NEWER
    assert sf.verify_image(img, current_version=4, crypto=backend).ok()


def test_anti_rollback_comparacion_sin_signo():
    """0xFFFFFFFF es una version legal: no se compara con signo."""
    img, backend = _firmar(version=0xFFFFFFFF)
    v = sf.verify_image(img, current_version=0xFFFFFFFE, crypto=backend)
    assert v.ok(), sf.reject_text(v.reject)
    assert v.version == 0xFFFFFFFF


# --- Rechazos: integridad ----------------------------------------------------

def test_manifesto_corrupto():
    """Manipular el manifiesto sin recalcular su CRC lo delata."""
    img, backend = _firmar()
    img = _mutable(img)
    img[0x09] ^= 0xFF          # version, sin tocar header_crc32
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.HEADER_CRC_FAIL
    assert sf.reject_text(v.reject) == "CRC de manifiesto incorrecto"
    # steps_done se incrementa DESPUES de pasar cada paso: 5 significa que
    # supero el anti-rollback y fallo al comprobar el CRC del manifiesto.
    assert v.steps_done == 5


def test_header_crc32_corrupto():
    """Romper el propio campo header_crc32 es manipulacion del manifiesto."""
    img, backend = _firmar()
    img = _mutable(img)
    sf.put32(img, 0x38, 0xDEADBEEF)
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.HEADER_CRC_FAIL


def test_payload_corrupto_un_bit():
    """Un solo bit en el payload lo detecta el CRC (paso 7)."""
    img, backend = _firmar()
    img = _mutable(img)
    img[sf.HEADER_LEN + 100] ^= 0x01
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.PAYLOAD_CRC_FAIL
    assert sf.reject_text(v.reject) == "CRC de payload incorrecto"
    assert v.steps_done == 6


def test_payload_con_crc_enganado():
    """Atacante que recalcula el CRC32 pero no toca el SHA-256: lo delata el hash.

    Es el motivo de tener los dos: el CRC es barato y un atacante lo puede
    recalcular a mano; el SHA-256 va firmado y no.
    """
    img, backend = _firmar()
    img = _mutable(img)
    img[sf.HEADER_LEN + 100] ^= 0x01
    sf.put32(img, 0x14, sf.crc32(img[sf.HEADER_LEN:sf.HEADER_LEN + 4096]))
    _recalcular_header_crc(img)
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.PAYLOAD_HASH_FAIL
    assert sf.reject_text(v.reject) == "SHA-256 de payload incorrecto"
    assert v.steps_done == 7


# --- Rechazos: firma ---------------------------------------------------------

def test_firma_corrupta():
    """Los 64 bytes ultimos tocados invalidan la imagen."""
    img, backend = _firmar()
    img = _mutable(img)
    img[-1] ^= 0xFF
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.SIGNATURE_INVALID
    assert sf.reject_text(v.reject) == "firma invalida"
    assert v.steps_done == 8


def test_firma_heuristica_rechazada():
    """64 bytes de relleno no son una firma, aunque el CRC y el hash cuadren."""
    img, backend = _firmar()
    img = _mutable(img)
    img[-64:] = bytes(0xFF) * 64
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.SIGNATURE_INVALID


def test_verificar_con_otra_clave_publica():
    """Imagen firmada por A, verificada con la publica de B: rechazada."""
    img, _ = _firmar()
    otra_backend = _backend()
    v = _verificar(img, otra_backend)
    assert v.reject == sf.FwReject.SIGNATURE_INVALID
    assert sf.reject_text(v.reject) == "firma invalida"
    assert v.steps_done == 8


def test_payload_reemplazado_conservando_manifesto():
    """Firmware distinto pegado en una imagen firmada: rechazado por la firma."""
    img, backend = _firmar()
    img = _mutable(img)
    nuevo = bytes((b ^ 0x5A) for b in _payload())
    img[sf.HEADER_LEN:sf.HEADER_LEN + 4096] = nuevo
    # El atacante actualiza CRC y SHA-256 y hasta el CRC del manifiesto:
    # solo la firma se lo impide.
    sf.put32(img, 0x14, sf.crc32(nuevo))
    img[0x18:0x38] = sf.crypto_sha256(nuevo)
    _recalcular_header_crc(img)
    v = _verificar(img, backend)
    assert v.reject == sf.FwReject.SIGNATURE_INVALID


# --- Fail-closed -------------------------------------------------------------

def test_sin_backend_se_rechaza_todo():
    """Sin CryptoBackend no se acepta ninguna imagen (FwReject en el C++)."""
    img, _ = _firmar()
    v = sf.verify_image(img, crypto=None)
    assert v.reject == sf.FwReject.NO_CRYPTO_BACKEND
    assert sf.reject_text(v.reject) == "sin backend criptografico"
    assert v.steps_done == 7


def test_backend_roto_se_rechaza():
    """Un backend que no puede calcular el hash no puede decir que si."""
    class Roto(sf.CryptoBackend):
        def sha256(self, data):
            return None

    img, backend = _firmar()
    roto = Roto(backend.public_key())
    v = sf.verify_image(img, crypto=roto)
    assert v.reject == sf.FwReject.NO_CRYPTO_BACKEND


def test_sin_clave_publica_se_rechaza():
    """publicKey() == nullptr es lo que hace el firmware rechazar."""
    class SinClave(sf.CryptoBackend):
        def public_key(self):
            return None

    img, backend = _firmar()
    v = sf.verify_image(img, crypto=SinClave(backend.public_key()))
    assert v.reject == sf.FwReject.SIGNATURE_INVALID


# --- Los bounds van antes que la firma (garantia de seguridad) ---------------

def test_firma_no_se_verifica_si_ya_se_rechazo():
    """Si un rechazo ocurre antes, la firma NO se toca: no se paga con basura.

    Es la garantia que hace aceptable verificar los bounds antes de la firma
    (docs/ota_procedure.md seccion 4). Aqui se comprueba de forma directa: el
    backend lanza excepcion si alguien llega a verificar la firma, y el test
    pasa solo si nunca se llega.
    """
    class Prohibido(sf.CryptoBackend):
        """Backend que EXPLOTA si alguien llega a verificar la firma."""

        def ed25519_verify(self, pubkey, msg, sig):
            raise AssertionError(
                "la firma no debe verificarse cuando ya se ha rechazado")

    class Contador(sf.CryptoBackend):
        """Delega en un backend real y cuenta las llamadas a la firma."""

        def __init__(self, real):
            super().__init__(real.public_key())
            self._real = real
            self.firmas = 0

        def ed25519_verify(self, pubkey, msg, sig):
            self.firmas += 1
            return self._real.ed25519_verify(pubkey, msg, sig)

    backend_prohibido = Prohibido(b"\x00" * 32)
    imagenes = []

    # magic
    img = _mutable(_firmar()[0])
    img[0] = ord("Z")
    imagenes.append(("magic", img))
    # header_len
    img = _mutable(_firmar()[0])
    sf.put32(img, 0x04, 32)
    imagenes.append(("header_len", img))
    # payload no cabe
    img = _mutable(_firmar()[0])
    sf.put32(img, 0x0C, 0xFFFFFFF0)
    _recalcular_header_crc(img)
    imagenes.append(("slot", img))
    # truncada
    imagenes.append(("truncada", _firmar()[0][:-10]))
    # version
    imagenes.append(("version", _firmar(version=5)[0]))
    # CRC del manifiesto
    img = _mutable(_firmar()[0])
    sf.put32(img, 0x38, 0xDEADBEEF)
    imagenes.append(("header_crc", img))
    # CRC del payload
    img = _mutable(_firmar()[0])
    img[sf.HEADER_LEN + 100] ^= 0x01
    imagenes.append(("payload_crc", img))
    # SHA-256 del payload
    img = _mutable(_firmar()[0])
    cuerpo = bytearray(img[sf.HEADER_LEN:sf.HEADER_LEN + 4096])
    cuerpo[100] ^= 0x01
    sf.put32(img, 0x14, sf.crc32(cuerpo))
    _recalcular_header_crc(img)
    imagenes.append(("payload_hash", img))

    for nombre, img in imagenes:
        v = sf.verify_image(img, current_version=VERSION,
                            crypto=backend_prohibido)
        assert not v.ok(), (nombre, "deberia haberse rechazado")
        assert v.steps_done <= 8, (nombre, v.steps_done)

    # Control positivo: una imagen buena SI tiene que llegar a la firma, y solo
    # una vez. Si esto no llegara, el test de arriba pasaria por lo equivocado.
    img, real = _firmar()
    contador = Contador(real)
    v = sf.verify_image(img, current_version=0, crypto=contador)
    assert v.ok(), sf.reject_text(v.reject)
    assert contador.firmas == 1


# --- Round-trip por la CLI ---------------------------------------------------

def _cli(*args):
    """Ejecuta la herramienta como subprocesso y devuelve (rc, stdout, stderr)."""
    proc = subprocess.run([sys.executable, TOOL_PATH] + list(args),
                          capture_output=True, text=True, check=False)
    return proc.returncode, proc.stdout, proc.stderr


def test_round_trip_build_id(tmp_path):
    """keygen -> sign -> verify: la version y el build-id vuelven intactos."""
    prefijo = str(tmp_path / "ota")
    rc, out, err = _cli("keygen", "--out-prefix", prefijo)
    assert rc == 0, err

    priv = prefijo + "_private.key"
    pub = prefijo + "_public.key"
    assert os.path.exists(priv) and os.path.exists(pub)

    # La privada tiene que ser 0600: es una clave de firma.
    modo = oct(os.stat(priv).st_mode & 0o777)
    assert modo == "0o600", modo

    fw = tmp_path / "supervisor.bin"
    fw.write_bytes(_payload(8192))
    imagen = tmp_path / "supervisor-v12.fwim"

    rc, out, err = _cli("sign", str(fw), "--version", "12",
                        "--build-id", "3735928559",   # 0xDEADBEEF
                        "--key", priv, "--out", str(imagen))
    assert rc == 0, err

    rc, out, err = _cli("verify", str(imagen), "--pubkey", pub,
                        "--json")
    assert rc == 0, err
    v = json.loads(out)
    assert v["ok"] is True
    assert v["reject"] == "OK"
    assert v["reject_code"] == "NONE"
    assert v["version"] == 12
    assert v["build_id"] == 3735928559
    assert v["payload_len"] == 8192
    assert v["total_len"] == 64 + 8192 + 64
    assert v["steps_done"] == 9


def test_cli_anti_rollback_por_min_version(tmp_path):
    """--min-version es la version ya instalada: filtra el downgrade."""
    prefijo = str(tmp_path / "ota")
    assert _cli("keygen", "--out-prefix", prefijo)[0] == 0
    fw = tmp_path / "fw.bin"
    fw.write_bytes(_payload(256))
    imagen = tmp_path / "fw.fwim"
    assert _cli("sign", str(fw), "--version", "5", "--key",
                prefijo + "_private.key", "--out", str(imagen))[0] == 0

    # Instalada la 5, la 5 es reinstalacion.
    rc, out, _ = _cli("verify", str(imagen), "--pubkey",
                      prefijo + "_public.key", "--min-version", "5", "--json")
    assert rc == 1
    assert json.loads(out)["reject"] == "version no mas nueva"

    # Instalada la 4, la 5 si es una actualizacion.
    rc, out, _ = _cli("verify", str(imagen), "--pubkey",
                      prefijo + "_public.key", "--min-version", "4", "--json")
    assert rc == 0
    assert json.loads(out)["ok"] is True


def test_cli_imagen_manipulada_falla(tmp_path):
    """La CI depende de esto: una imagen tocada tiene que dar codigo 1."""
    prefijo = str(tmp_path / "ota")
    assert _cli("keygen", "--out-prefix", prefijo)[0] == 0
    fw = tmp_path / "fw.bin"
    fw.write_bytes(_payload(1024))
    imagen = tmp_path / "fw.fwim"
    assert _cli("sign", str(fw), "--version", "3", "--key",
                prefijo + "_private.key", "--out", str(imagen))[0] == 0

    pub = prefijo + "_public.key"
    assert _cli("verify", str(imagen), "--pubkey", pub)[0] == 0

    # Mismo firmware, otra clave de firma: debe fallar.
    otro = str(tmp_path / "otro")
    assert _cli("keygen", "--out-prefix", otro)[0] == 0
    rc, out, _ = _cli("verify", str(imagen), "--pubkey",
                      otro + "_public.key", "--json")
    assert rc == 1
    assert json.loads(out)["reject"] == "firma invalida"


def test_cli_sign_rechaza_payload_vacio(tmp_path):
    """Un payload de 0 bytes lo rechazaria el firmware; no se firma."""
    prefijo = str(tmp_path / "ota")
    assert _cli("keygen", "--out-prefix", prefijo)[0] == 0
    vacio = tmp_path / "vacio.bin"
    vacio.write_bytes(b"")
    rc, _out, err = _cli("sign", str(vacio), "--version", "1", "--key",
                         prefijo + "_private.key", "--out",
                         str(tmp_path / "salida.fwim"))
    assert rc == 2
    assert "vacio" in err


def test_cli_sin_subcomando_ayuda():
    """Sin subcomando no se ejecuta nada: se muestra la ayuda."""
    proc = subprocess.run([sys.executable, TOOL_PATH],
                          capture_output=True, text=True, check=False)
    assert proc.returncode == 2
    assert "sign" in proc.stderr and "verify" in proc.stderr


def test_textos_de_rechazo_coinciden_con_el_cpp():
    """Los textos deben ser LITERALES los de fwRejectText() en fw_image.cpp.

    Se releen del fichero C++ para que una edicion de este modulo no pueda
    desviarse en silencio: el dashboard y el firmware muestran el mismo texto.
    """
    cpp = os.path.join(REPO_ROOT, "mcu", "src", "fw_image.cpp")
    with open(cpp, encoding="utf-8") as fh:
        fuente = fh.read()

    for reject, texto in sf.REJECT_TEXT.items():
        if reject == sf.FwReject.NONE:
            continue
        esperado = 'return "%s";' % texto
        assert esperado in fuente, ("el texto %r no aparece en fw_image.cpp"
                                    % texto)
    assert 'return "OK";' in fuente


if __name__ == '__main__':
    sys.exit(pytest.main([__file__, "-v"]))