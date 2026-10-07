#!/usr/bin/env python3
"""
@file tools/sign_firmware.py
@brief Firmar y verificar imagenes de firmware OTA (Fase 4, paso 1)

Espejo EXACTO del pipeline de verificacion de `mcu/src/fw_image.cpp`. Si el
formato o el orden de los pasos cambian en el C++, hay que cambiarlos aqui: una
herramienta que firma un formato distinto del que el MCU verifica es peor que
no tener herramienta, porque aparenta haber un control donde no lo hay.

Formato de la imagen (docs/ota_procedure.md seccion 4):

    0x00  4   magic           0x4D495746
    0x04  4   header_len      = 64
    0x08  4   version         monotonica, nunca decrece
    0x0C  4   payload_len     bytes de firmware
    0x10  4   build_id        git short sha o timestamp
    0x14  4   payload_crc32   CRC-32/ISO-HDLC del payload
    0x18  32  payload_sha256  SHA-256 del payload
    0x38  4   header_crc32    CRC-32 de los bytes [0x00, 0x38)
    0x3C  4   reserved        0
    0x40  N   payload
    0x40+N 64 signature       Ed25519 de [0x00, 64 + payload_len)

ORDEN DE ARMADO (importante; alterarlo firma mal y en silencio):

    1. se rellenan los bytes [0x00, 0x38) del manifiesto
    2. header_crc32 = crc32 de esos 56 bytes, escrito en 0x38
    3. reserved = 0 en 0x3C   -> manifiesto COMPLETO de 64 bytes
    4. la firma se calcula sobre `manifiesto_completo || payload`
    5. la firma (64 bytes) se anexa AL FINAL

El paso 4 incluye header_crc32 y reserved porque la firma cubre los primeros
`header_len + payload_len` bytes de la imagen, igual que hace el firmware con
`ed25519_verify(pubkey, image, m.header_len + m.payload_len, sig)`.

ORDEN DE VERIFICACION (identico al de fw_image.cpp):

    magic -> header_len -> payload_len cabe en el slot -> longitud total sin
    overflow ni truncamiento -> version monotonica -> CRC del manifiesto ->
    CRC del payload -> SHA-256 del payload -> firma Ed25519

Los bounds van primero porque SOLO rechazan: evitan un desbordamiento de buffer
con una imagen manipulada y no aceptan nada en funcion de datos sin verificar.
La firma va al final porque es lo mas caro (decenas de miles de operaciones) y
no tiene sentido pagarla con basura de red.

La criptografia nunca se implementa a mano: se usa `cryptography` y, si no esta,
`PyNaCl`. Sin ninguna de las dos la herramienta NO firma ni verifica y sale con
codigo 2, que es el equivalente en Python del NO_CRYPTO_BACKEND del firmware
(fail-closed: es peor no firmar que firmar sin comprobar).
"""

import argparse
import base64
import json
import os
import struct
import sys
import zlib

# --- Constantes del formato (mcu/include/fw_image.h) ------------------------

MAGIC = 0x4D495746
HEADER_LEN = 64
SIGNATURE_LEN = 64
HEADER_CRC_END = 0x38          # header_crc32 cubre [0x00, 0x38)
SHA256_LEN = 32
UINT32_MAX = 0xFFFFFFFF

# Slot de flash por defecto (512 KiB), el mismo que usa el test nativo.
DEFAULT_SLOT_MAX = 512 * 1024

# Codigos de salida de la CLI.
EXIT_OK = 0
EXIT_VERIFY_FAIL = 1
EXIT_USAGE = 2


# --- Codigos de rechazo (copia de `enum class FwReject`) ---------------------

class FwReject:
    """Mismos valores y mismo orden que `enum class FwReject` de fw_image.h."""

    NONE = 0
    TOO_SMALL = 1
    BAD_MAGIC = 2
    BAD_HEADER_LEN = 3
    PAYLOAD_TOO_BIG = 4
    IMAGE_TRUNCATED = 5
    IMAGE_TOO_BIG = 6
    VERSION_NOT_NEWER = 7
    HEADER_CRC_FAIL = 8
    PAYLOAD_CRC_FAIL = 9
    PAYLOAD_HASH_FAIL = 10
    NO_CRYPTO_BACKEND = 11
    SIGNATURE_INVALID = 12

    _NAMES = {
        NONE: "NONE",
        TOO_SMALL: "TOO_SMALL",
        BAD_MAGIC: "BAD_MAGIC",
        BAD_HEADER_LEN: "BAD_HEADER_LEN",
        PAYLOAD_TOO_BIG: "PAYLOAD_TOO_BIG",
        IMAGE_TRUNCATED: "IMAGE_TRUNCATED",
        IMAGE_TOO_BIG: "IMAGE_TOO_BIG",
        VERSION_NOT_NEWER: "VERSION_NOT_NEWER",
        HEADER_CRC_FAIL: "HEADER_CRC_FAIL",
        PAYLOAD_CRC_FAIL: "PAYLOAD_CRC_FAIL",
        PAYLOAD_HASH_FAIL: "PAYLOAD_HASH_FAIL",
        NO_CRYPTO_BACKEND: "NO_CRYPTO_BACKEND",
        SIGNATURE_INVALID: "SIGNATURE_INVALID",
    }


# Textos de rechazo: COPIA LITERAL de `fwRejectText()` en fw_image.cpp. Sin
# acentos y tal cual, porque el dashboard muestra el mismo texto que llega del
# firmware y una diferencia de un acento rompe el grep del operador.
REJECT_TEXT = {
    FwReject.NONE: "OK",
    FwReject.TOO_SMALL: "imagen menor que el manifiesto",
    FwReject.BAD_MAGIC: "magic invalido",
    FwReject.BAD_HEADER_LEN: "tamano de manifiesto inesperado",
    FwReject.PAYLOAD_TOO_BIG: "payload no cabe en el slot",
    FwReject.IMAGE_TRUNCATED: "imagen truncada",
    FwReject.IMAGE_TOO_BIG: "tamano total desbordado",
    FwReject.VERSION_NOT_NEWER: "version no mas nueva",
    FwReject.HEADER_CRC_FAIL: "CRC de manifiesto incorrecto",
    FwReject.PAYLOAD_CRC_FAIL: "CRC de payload incorrecto",
    FwReject.PAYLOAD_HASH_FAIL: "SHA-256 de payload incorrecto",
    FwReject.NO_CRYPTO_BACKEND: "sin backend criptografico",
    FwReject.SIGNATURE_INVALID: "firma invalida",
}

FALLBACK_TEXT = "error desconocido"


def reject_text(reject):
    """Equivalente a `fwRejectText()`."""
    return REJECT_TEXT.get(reject, FALLBACK_TEXT)


def reject_name(reject):
    """Nombre del enum, para diagnostico (FwReject.BAD_MAGIC -> 'BAD_MAGIC')."""
    return FwReject._NAMES.get(reject, "UNKNOWN")


# --- Utilidades de bajo nivel -----------------------------------------------

def crc32(data):
    """CRC-32/ISO-HDLC: el mismo algoritmo que `fwCrc32()` del firmware.

    `zlib.crc32` ES ese algoritmo (semilla 0xFFFFFFFF, polinomio reflejado
    0xEDB88320, xor final), asi que no hace falta reimplementarlo ni auditarlo
    aparte. El test nativo contrasta los mismos vectores.
    """
    return zlib.crc32(data) & UINT32_MAX


def crypto_sha256(data):
    """SHA-256 (FIPS 180-4) de `data`. Lo que mete el backend del firmware."""
    import hashlib
    return hashlib.sha256(data).digest()


def put32(buf, offset, value):
    """Escribe un uint32 little-endian. `buf` debe ser un bytearray."""
    struct.pack_into("<I", buf, offset, value & UINT32_MAX)


def rd32(data, offset):
    """Lee un uint32 little-endian. Equivale a `rd32()` del firmware."""
    return struct.unpack_from("<I", data, offset)[0]


class FwManifest:
    """Copia de `struct FwManifest`. Solo deserializa: no verifica nada."""

    __slots__ = ("magic", "header_len", "version", "payload_len", "build_id",
                 "payload_crc32", "payload_sha256", "header_crc32", "reserved")

    def __init__(self):
        self.magic = 0
        self.header_len = 0
        self.version = 0
        self.payload_len = 0
        self.build_id = 0
        self.payload_crc32 = 0
        self.payload_sha256 = b"\x00" * SHA256_LEN
        self.header_crc32 = 0
        self.reserved = 0

    def total_length(self):
        """`fwTotalLength()`: manifiesto + payload + firma."""
        return self.header_len + self.payload_len + SIGNATURE_LEN


def parse_manifest(image):
    """`fwParseManifest()`: deserializa sin verificar nada."""
    m = FwManifest()
    if image is None or len(image) < HEADER_LEN:
        return m
    m.magic = rd32(image, 0x00)
    m.header_len = rd32(image, 0x04)
    m.version = rd32(image, 0x08)
    m.payload_len = rd32(image, 0x0C)
    m.build_id = rd32(image, 0x10)
    m.payload_crc32 = rd32(image, 0x14)
    m.payload_sha256 = bytes(image[0x18:0x18 + SHA256_LEN])
    m.header_crc32 = rd32(image, 0x38)
    m.reserved = rd32(image, 0x3C)
    return m


class FwVerdict:
    """Copia de `struct FwVerdict`."""

    __slots__ = ("reject", "version", "payload_len", "build_id", "total_len",
                 "steps_done")

    def __init__(self):
        self.reject = FwReject.NONE
        self.version = 0
        self.payload_len = 0
        self.build_id = 0
        self.total_len = 0
        self.steps_done = 0

    def ok(self):
        return self.reject == FwReject.NONE

    def to_dict(self):
        """Los campos del veredicto, tal como los expone el C++ (para CI)."""
        return {
            "reject": reject_text(self.reject),
            "reject_code": reject_name(self.reject),
            "version": self.version,
            "payload_len": self.payload_len,
            "build_id": self.build_id,
            "total_len": self.total_len,
            "steps_done": self.steps_done,
            "ok": self.ok(),
        }


# --- Backends criptograficos ------------------------------------------------

BACKEND_CRYPT = "cryptography"
BACKEND_PYNACL = "pynacl"

NO_BACKEND_MSG = (
    "sin backend criptografico: falta una libreria Ed25519\n"
    "  pip install cryptography      # preferido\n"
    "  pip install pynacl            # alternativa\n"
    "Sin una de las dos NO se puede firmar ni verificar (fail-closed, igual "
    "que el firmware sin CryptoBackend)."
)


def backend_disponible():
    """Nombre del backend criptografico disponible, o None si no hay ninguno.

    `cryptography` tiene prioridad: es la opcion de sistema, trae PEM y es la
    que instala el CI. PyNaCl es el plan B para maquinas sin ella.
    """
    try:
        import cryptography  # noqa: F401  (solo se comprueba la presencia)
        return BACKEND_CRYPT
    except ImportError:
        pass
    try:
        import nacl  # noqa: F401
        return BACKEND_PYNACL
    except ImportError:
        return None


HAVE_CRYPTO = backend_disponible() is not None


class CryptoBackend:
    """Equivalente a la clase `CryptoBackend` de fw_image.h.

    Se conserva la misma division (publicKey / sha256 / ed25519_verify) para que
    el orden de llamadas sea comparable linea a linea con el C++, y para que un
    doble de test pueda sustituirla desde las pruebas.
    """

    def __init__(self, pubkey_raw, priv=None):
        self._pub = pubkey_raw
        self._priv = priv

    def public_key(self):
        return self._pub

    def sha256(self, data):
        return crypto_sha256(data)

    def ed25519_verify(self, pubkey, msg, sig):
        raise NotImplementedError

    def ed25519_sign(self, msg):
        raise NotImplementedError


class CryptographyBackend(CryptoBackend):
    """Backend sobre `cryptography` (preferido: PEM disponible)."""

    def ed25519_verify(self, pubkey, msg, sig):
        from cryptography.exceptions import InvalidSignature
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PublicKey)
        try:
            Ed25519PublicKey.from_public_bytes(pubkey).verify(sig, msg)
            return True
        except InvalidSignature:
            return False

    def ed25519_sign(self, msg):
        return self._priv.sign(msg)


class NaclBackend(CryptoBackend):
    """Backend sobre PyNaCl, para maquinas sin `cryptography`."""

    def ed25519_verify(self, pubkey, msg, sig):
        import nacl.signing
        try:
            nacl.signing.VerifyKey(pubkey).verify(msg, sig)
            return True
        except (ValueError, TypeError):
            return False

    def ed25519_sign(self, msg):
        import nacl.signing
        return nacl.signing.SigningKey(self._priv).sign(msg).signature


BACKEND_CLASS = {
    BACKEND_CRYPT: CryptographyBackend,
    BACKEND_PYNACL: NaclBackend,
}


# --- Claves en disco --------------------------------------------------------

# Formato crudo de reserva para cuando la libreria no da PEM: un comentario de
# cabecera reconocible y luego la clave en base64.
RAW_MARKER = b"# ota-ed25519-raw-v1"
RAW_HEADER = (b"# ota-ed25519-raw-v1\n"
              b"# Clave Ed25519 de 32 bytes en base64.\n"
              b"# Formato de reserva: usalo solo si no hay 'cryptography'.\n")


def generar_claves():
    """Genera un par Ed25519.

    @return (backend, pubkey_raw32, bytes_privados, bytes_publicos)
    """
    nombre = backend_disponible()
    if nombre is None:
        raise RuntimeError(NO_BACKEND_MSG)

    if nombre == BACKEND_CRYPT:
        from cryptography.hazmat.primitives import serialization
        from cryptography.hazmat.primitives.asymmetric.ed25519 import (
            Ed25519PrivateKey)

        priv = Ed25519PrivateKey.generate()
        pub = priv.public_key()
        pub_raw = pub.public_bytes(encoding=serialization.Encoding.Raw,
                                   format=serialization.PublicFormat.Raw)
        priv_pem = priv.private_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PrivateFormat.PKCS8,
            encryption_algorithm=serialization.NoEncryption())
        pub_pem = pub.public_bytes(
            encoding=serialization.Encoding.PEM,
            format=serialization.PublicFormat.SubjectPublicKeyInfo)
        return CryptographyBackend(pub_raw, priv), pub_raw, priv_pem, pub_pem

    import nacl.signing
    sk = nacl.signing.SigningKey.generate()
    pub_raw = bytes(sk.verify_key.encode())
    priv_raw = RAW_HEADER + base64.b64encode(sk.encode()) + b"\n"
    pub_file = RAW_HEADER + base64.b64encode(pub_raw) + b"\n"
    return NaclBackend(pub_raw, sk.encode()), pub_raw, priv_raw, pub_file


def _es_pem(datos):
    return b"-----BEGIN" in datos[:256]


def _cargar_privada(datos, backend_nombre):
    """Clave privada serializada -> objeto que sepa firmar."""
    if _es_pem(datos):
        if backend_nombre != BACKEND_CRYPT:
            raise ValueError("la clave privada es PEM pero el backend "
                             "disponible es '%s'; instala 'cryptography' o "
                             "genera la clave en el formato crudo" %
                             backend_nombre)
        from cryptography.hazmat.primitives import serialization
        return serialization.load_pem_private_key(datos, password=None)

    if datos.startswith(RAW_MARKER):
        lineas = [l for l in datos.splitlines()
                  if l.strip() and not l.lstrip().startswith(b"#")]
        semilla = base64.b64decode(b"".join(lineas))
        if len(semilla) != 32:
            raise ValueError("la semilla Ed25519 no mide 32 bytes (%d)" %
                             len(semilla))
        if backend_nombre == BACKEND_CRYPT:
            from cryptography.hazmat.primitives.asymmetric.ed25519 import (
                Ed25519PrivateKey)
            return Ed25519PrivateKey.from_private_bytes(semilla)
        return semilla

    raise ValueError("formato de clave privada no reconocido (se esperaba PEM "
                     "PKCS#8 o el formato crudo ota-ed25519-raw-v1)")


def _cargar_publica(datos, backend_nombre):
    """Clave publica -> 32 bytes crudos.

    Acepta PEM, el formato crudo propio y 32 bytes en crudo, hex o base64,
    que es como se compila en `mcu/include/ota_pubkey.h`.
    """
    if _es_pem(datos):
        if backend_nombre != BACKEND_CRYPT:
            raise ValueError("la clave publica es PEM pero el backend "
                             "disponible es '%s'; instala 'cryptography'" %
                             backend_nombre)
        from cryptography.hazmat.primitives import serialization
        clave = serialization.load_pem_public_key(datos)
        return clave.public_bytes(encoding=serialization.Encoding.Raw,
                                  format=serialization.PublicFormat.Raw)

    if datos.startswith(RAW_MARKER):
        lineas = [l for l in datos.splitlines()
                  if l.strip() and not l.lstrip().startswith(b"#")]
        crudo = base64.b64decode(b"".join(lineas))
        if len(crudo) != SHA256_LEN:
            raise ValueError("la clave publica no mide 32 bytes (%d)" %
                             len(crudo))
        return crudo

    texto = b"".join(datos.split())
    if len(texto) == 64:
        try:
            return bytes.fromhex(texto.decode("ascii"))
        except (ValueError, UnicodeDecodeError):
            pass
    try:
        crudo = base64.b64decode(texto, validate=True)
        if len(crudo) == SHA256_LEN:
            return crudo
    except ValueError:
        pass
    if len(texto) == SHA256_LEN:
        return texto
    raise ValueError("formato de clave publica no reconocido (se esperaba "
                     "PEM, hex de 32 bytes o base64 de 32 bytes)")


def leer_clave_privada(ruta, backend_nombre):
    """Lee la clave privada de disco."""
    with open(ruta, "rb") as fh:
        datos = fh.read()
    if not datos.strip():
        raise ValueError("la clave privada '%s' esta vacia" % ruta)
    return _cargar_privada(datos, backend_nombre)


def leer_clave_publica(ruta, backend_nombre):
    """Lee la clave publica de disco -> 32 bytes crudos."""
    with open(ruta, "rb") as fh:
        datos = fh.read()
    if not datos.strip():
        raise ValueError("la clave publica '%s' esta vacia" % ruta)
    return _cargar_publica(datos, backend_nombre)


def _pubkey_de_privada(priv, backend_nombre):
    """Clave publica (32 bytes) derivada de la privada."""
    if backend_nombre == BACKEND_CRYPT:
        from cryptography.hazmat.primitives import serialization
        return priv.public_key().public_bytes(
            encoding=serialization.Encoding.Raw,
            format=serialization.PublicFormat.Raw)
    import nacl.signing
    return bytes(nacl.signing.SigningKey(priv).verify_key.encode())


# --- Firma ------------------------------------------------------------------

def build_image(payload, version, build_id, backend):
    """Arma la imagen firmada respetando el ORDEN DE ARMADO del modulo.

    @param payload  bytes del firmware
    @param version  version monotonica (uint32)
    @param build_id git short sha o timestamp (uint32)
    @param backend  CryptoBackend con la clave privada cargada
    @return bytes de la imagen completa: manifiesto || payload || firma
    """
    payload_len = len(payload)
    if payload_len == 0:
        raise ValueError("el payload esta vacio; el firmware lo rechazaria "
                         "con 'payload no cabe en el slot'")
    if payload_len > DEFAULT_SLOT_MAX:
        raise ValueError("el payload (%d bytes) supera el slot por defecto "
                         "(%d bytes)" % (payload_len, DEFAULT_SLOT_MAX))
    if not 0 <= version <= UINT32_MAX:
        raise ValueError("la version debe caber en un uint32")
    if not 0 <= build_id <= UINT32_MAX:
        raise ValueError("el build-id debe caber en un uint32")

    # 1. Solo los bytes [0x00, 0x38); el resto queda a 0.
    header = bytearray(HEADER_LEN)
    # El magic se escribe solo, con la constante numerica: 0x4D495746 en
    # little-endian son los bytes 46 57 49 4D = "FWIM". Escribirlo a mano
    # seria la forma facil de que el valor y el comentario discrepen.
    put32(header, 0x00, MAGIC)
    put32(header, 0x04, HEADER_LEN)
    put32(header, 0x08, version)
    put32(header, 0x0C, payload_len)
    put32(header, 0x10, build_id)
    put32(header, 0x14, crc32(payload))
    header[0x18:0x18 + SHA256_LEN] = backend.sha256(payload)

    # 2. header_crc32 sobre esos 56 bytes, escrito en 0x38.
    put32(header, 0x38, crc32(bytes(header[0:HEADER_CRC_END])))
    # 3. reserved = 0 en 0x3C. Manifiesto COMPLETO.
    put32(header, 0x3C, 0)

    # 4. La firma cubre el manifiesto COMPLETO mas el payload, que son los
    #    primeros header_len + payload_len bytes de la imagen.
    firmado = bytes(header) + payload
    firma = backend.ed25519_sign(firmado)
    if len(firma) != SIGNATURE_LEN:
        raise ValueError("la firma no mide %d bytes (%d)" %
                         (SIGNATURE_LEN, len(firma)))

    # 5. La firma va AL FINAL.
    return firmado + firma


# --- Verificacion (espejo de fwVerifyImage) ---------------------------------

def verify_image(image, slot_max=DEFAULT_SLOT_MAX, current_version=0,
                 crypto=None):
    """`fwVerifyImage()` en Python: mismo orden y mismos rechazos.

    @param image           bytes de la imagen (manifiesto + payload + firma)
    @param slot_max        tamano del slot de flash donde se escribiria
    @param current_version version ya instalada (anti-rollback)
    @param crypto          CryptoBackend; None => sin backend, se rechaza todo
    @return FwVerdict
    """
    v = FwVerdict()

    # --- 1. Bounds minimos: ¿hay siquiera un manifiesto completo? ---
    if image is None or len(image) < HEADER_LEN:
        v.reject = FwReject.TOO_SMALL
        return v
    v.steps_done = 1

    m = parse_manifest(image)
    v.version = m.version
    v.payload_len = m.payload_len
    v.build_id = m.build_id

    # --- 2. Identidad de la imagen ---
    if m.magic != MAGIC:
        v.reject = FwReject.BAD_MAGIC
        return v
    if m.header_len != HEADER_LEN:
        v.reject = FwReject.BAD_HEADER_LEN
        return v
    v.steps_done = 2

    # --- 3. ¿El payload cabe en el slot? (bounds: solo rechaza) ---
    if m.payload_len == 0 or m.payload_len > slot_max:
        v.reject = FwReject.PAYLOAD_TOO_BIG
        return v
    v.steps_done = 3

    # --- 4. Overflow y truncamiento ---
    # payload_len ya esta acotado por slot_max, asi que la suma no puede
    # desbordar; aun asi se comprueba para no confiar en el indice.
    total = m.header_len + m.payload_len + SIGNATURE_LEN
    if total > UINT32_MAX:
        v.reject = FwReject.IMAGE_TOO_BIG
        return v
    if total > len(image):
        v.reject = FwReject.IMAGE_TRUNCATED
        return v
    v.total_len = total
    v.steps_done = 4

    # --- 5. Anti-rollback: comparacion sin signo (0xFFFFFFFF es legal) ---
    if m.version <= current_version:
        v.reject = FwReject.VERSION_NOT_NEWER
        return v
    v.steps_done = 5

    # --- 6. CRC del manifiesto (cubre los bytes 0x00..0x37) ---
    if crc32(image[0:HEADER_CRC_END]) != m.header_crc32:
        v.reject = FwReject.HEADER_CRC_FAIL
        return v
    v.steps_done = 6

    payload = image[m.header_len:m.header_len + m.payload_len]
    inicio_firma = m.header_len + m.payload_len
    sig = image[inicio_firma:inicio_firma + SIGNATURE_LEN]

    # --- 7. CRC del payload ---
    if crc32(payload) != m.payload_crc32:
        v.reject = FwReject.PAYLOAD_CRC_FAIL
        return v
    v.steps_done = 7

    # --- 8. Fail-closed: sin backend no se acepta nada ---
    if crypto is None:
        v.reject = FwReject.NO_CRYPTO_BACKEND
        return v

    digest = crypto.sha256(payload)
    if digest is None:
        v.reject = FwReject.NO_CRYPTO_BACKEND
        return v
    if not ct_equal(digest, m.payload_sha256, SHA256_LEN):
        v.reject = FwReject.PAYLOAD_HASH_FAIL
        return v
    v.steps_done = 8

    # --- 9. Firma Ed25519 (la mas cara, la ultima) ---
    pubkey = crypto.public_key()
    if pubkey is None or not crypto.ed25519_verify(
            pubkey, image[0:m.header_len + m.payload_len], sig):
        v.reject = FwReject.SIGNATURE_INVALID
        return v
    v.steps_done = 9

    v.reject = FwReject.NONE
    return v


def ct_equal(a, b, length):
    """Comparacion en tiempo constante, como `ct_equal()` del firmware.

    Las longitudes son fijas y publicas, asi que comparar byte a byte con
    acumulador no filtra informacion, pero el acumulador evita que el
    optimizador saque el bucle. Se mantiene el criterio del C++ para que las
    dos implementaciones no diverjan.
    """
    if len(a) != length or len(b) != length:
        return False
    diff = 0
    for i in range(length):
        diff |= a[i] ^ b[i]
    return diff == 0


# --- Subcomandos ------------------------------------------------------------

def _leer_binario(ruta):
    with open(ruta, "rb") as fh:
        return fh.read()


def cmd_keygen(args):
    """Genera un par Ed25519: `<prefijo>_private.key` y `<prefijo>_public.key`."""
    nombre = backend_disponible()
    if nombre is None:
        print(NO_BACKEND_MSG, file=sys.stderr)
        return EXIT_USAGE

    prefijo = args.out_prefix
    os.makedirs(os.path.dirname(prefijo) or ".", exist_ok=True)
    priv_path = prefijo + "_private.key"
    pub_path = prefijo + "_public.key"

    for ruta in (priv_path, pub_path):
        if os.path.exists(ruta) and not args.force:
            print("error: '%s' ya existe (usa --force para sobrescribir)" %
                  ruta, file=sys.stderr)
            return EXIT_USAGE

    _, pub_raw, priv_datos, pub_datos = generar_claves()

    # La privada va 0600 SIEMPRE, y se abre con O_CREAT|O_EXCL para que el
    # permiso no dependa de un chmod posterior ni del umask.
    flags = os.O_WRONLY | os.O_CREAT | os.O_TRUNC
    if not args.force:
        flags |= os.O_EXCL
    fd = os.open(priv_path, flags, 0o600)
    with os.fdopen(fd, "wb") as fh:
        fh.write(priv_datos)
    os.chmod(priv_path, 0o600)

    with open(pub_path, "wb") as fh:
        fh.write(pub_datos)

    print("clave privada: %s (permisos 0600) - NO LA COMMITEES" % priv_path)
    print("clave publica: %s" % pub_path)
    print("backend: %s (formato: %s)" % (
        nombre, "PEM" if nombre == BACKEND_CRYPT else "crudo base64"))
    print("pubkey publica en hex, para mcu/include/ota_pubkey.h:")
    print("  %s" % pub_raw.hex())
    return EXIT_OK


def cmd_sign(args):
    """Firma un binario de firmware y escribe la imagen .fwim."""
    nombre = backend_disponible()
    if nombre is None:
        print(NO_BACKEND_MSG, file=sys.stderr)
        return EXIT_USAGE

    try:
        payload = _leer_binario(args.firmware)
        priv = leer_clave_privada(args.key, nombre)
    except (OSError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_USAGE

    backend = BACKEND_CLASS[nombre](_pubkey_de_privada(priv, nombre), priv)

    try:
        imagen = build_image(payload, args.version, args.build_id, backend)
    except ValueError as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_USAGE

    try:
        with open(args.out, "wb") as fh:
            fh.write(imagen)
    except OSError as exc:
        print("error: no se pudo escribir '%s': %s" % (args.out, exc),
              file=sys.stderr)
        return EXIT_USAGE

    # Autocomprobacion: si lo que se acaba de escribir no verifica, el binario
    # es inservible y no queremos publicarlo como si fuera bueno.
    v = verify_image(imagen, slot_max=args.slot_max, current_version=0,
                     crypto=backend)
    if not v.ok():
        print("error: la imagen firmada no verifica (%s)" % reject_text(v.reject),
              file=sys.stderr)
        return EXIT_VERIFY_FAIL

    print("firmada: %s (%d bytes)" % (args.out, len(imagen)))
    print("  version %d, build_id 0x%08X, payload %d bytes"
          % (args.version, args.build_id, len(payload)))
    print("  64 de manifiesto + %d de payload + 64 de firma" % len(payload))
    return EXIT_OK


def cmd_verify(args):
    """Verifica una imagen .fwim contra una clave publica."""
    if backend_disponible() is None:
        # Fail-closed tambien aqui: sin backend no se puede afirmar que una
        # imagen es buena. Con --json se responde igual, para que CI pueda
        # leer el motivo sin parsear texto libre.
        if args.json:
            v = FwVerdict()
            v.reject = FwReject.NO_CRYPTO_BACKEND
            print(json.dumps(v.to_dict()))
        else:
            print(NO_BACKEND_MSG, file=sys.stderr)
        return EXIT_USAGE

    nombre = backend_disponible()
    try:
        pub_raw = leer_clave_publica(args.pubkey, nombre)
    except (OSError, ValueError) as exc:
        print("error: %s" % exc, file=sys.stderr)
        return EXIT_USAGE

    try:
        image = _leer_binario(args.imagen)
    except OSError as exc:
        print("error: no se pudo leer '%s': %s" % (args.imagen, exc),
              file=sys.stderr)
        return EXIT_USAGE

    backend = BACKEND_CLASS[nombre](pub_raw)
    v = verify_image(image, slot_max=args.slot_max,
                     current_version=args.min_version, crypto=backend)

    if args.json:
        print(json.dumps(v.to_dict()))
    elif v.ok():
        print("OK %s: version %d, build_id 0x%08X, payload %d bytes, total %d"
              % (args.imagen, v.version, v.build_id, v.payload_len, v.total_len))
    else:
        print("RECHAZADA %s: %s (%s, paso %d de 9)"
              % (args.imagen, reject_text(v.reject), reject_name(v.reject),
                 v.steps_done))
    return EXIT_OK if v.ok() else EXIT_VERIFY_FAIL


# --- CLI --------------------------------------------------------------------

def build_parser():
    """Parser de la CLI, con la ayuda en español."""
    p = argparse.ArgumentParser(
        prog="sign_firmware.py",
        description=("Firma y verifica imagenes de firmware OTA con el mismo "
                     "formato y el mismo pipeline de rechazo que "
                     "mcu/src/fw_image.cpp."),
        epilog=("Codigos de salida: 0 correcto, 1 verificacion fallida, "
                "2 error de uso o de dependencias."),
        formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = p.add_subparsers(dest="comando", metavar="{sign,verify,keygen}")

    ps = sub.add_parser(
        "sign", help="firmar un binario de firmware",
        description="Firma un binario y escribe la imagen .fwim.")
    ps.add_argument("firmware", help="binario del firmware a firmar")
    ps.add_argument("--version", type=int, required=True, metavar="N",
                    help="version monotonica de la imagen")
    ps.add_argument("--build-id", type=int, default=0, metavar="N",
                    help="git short sha o timestamp Unix (por defecto 0)")
    ps.add_argument("--key", required=True, metavar="CLAVE",
                    help="clave privada Ed25519 (PEM o formato crudo)")
    ps.add_argument("--out", required=True, metavar="IMAGEN",
                    help="fichero de salida, p.ej. supervisor-v12.fwim")
    ps.add_argument("--slot-max", type=int, default=DEFAULT_SLOT_MAX,
                    metavar="N",
                    help="slot de flash para la autocomprobacion (por defecto "
                         "%d)" % DEFAULT_SLOT_MAX)
    ps.set_defaults(func=cmd_sign)

    pv = sub.add_parser(
        "verify", help="verificar una imagen firmada",
        description=("Verifica una imagen .fwim replicando el pipeline de "
                     "fw_image.cpp, con los bounds antes que la firma."))
    pv.add_argument("imagen", help="imagen .fwim a verificar")
    pv.add_argument("--pubkey", required=True, metavar="CLAVE",
                    help="clave publica Ed25519")
    pv.add_argument("--slot-max", type=int, default=DEFAULT_SLOT_MAX,
                    metavar="N",
                    help="tamano del slot de flash en bytes (por defecto %d)"
                         % DEFAULT_SLOT_MAX)
    pv.add_argument("--min-version", type=int, default=0, metavar="N",
                    help="version ya instalada; la imagen debe ser "
                         "ESTRICTAMENTE mayor (por defecto 0)")
    pv.add_argument("--json", action="store_true",
                    help="imprime el veredicto como JSON para CI")
    pv.set_defaults(func=cmd_verify)

    pk = sub.add_parser(
        "keygen", help="generar un par de claves Ed25519",
        description=("Genera un par Ed25519. La privada se escribe con "
                     "permisos 0600 y no debe commitearse nunca."))
    pk.add_argument("--out-prefix", required=True, metavar="PREFIJO",
                    help=("prefijo de salida: genera PREFIJO_private.key y "
                          "PREFIJO_public.key, p.ej. keys/ota"))
    pk.add_argument("--force", action="store_true",
                    help="sobrescribe ficheros existentes")
    pk.set_defaults(func=cmd_keygen)

    return p


def main(argv=None):
    parser = build_parser()
    args = parser.parse_args(argv)
    if getattr(args, "func", None) is None:
        parser.print_help(sys.stderr)
        return EXIT_USAGE
    try:
        return args.func(args)
    except KeyboardInterrupt:  # pragma: no cover
        print("interrumpido", file=sys.stderr)
        return EXIT_USAGE


if __name__ == "__main__":
    sys.exit(main())