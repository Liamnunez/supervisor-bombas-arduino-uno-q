# Herramientas de firmware firmado (Fase 4, paso 1)

`sign_firmware.py` firma y verifica imagenes de firmware OTA. Es el
equivalente en Python de `mcu/src/fw_image.cpp`: **mismo formato de manifiesto
y mismo pipeline de rechazo, en el mismo orden**.

Si los dos dejaran de coincidir, el CI firmaria algo que el MCU no acepta (o,
peor, al reves: el MCU aceptaria algo que el pipeline de CI Approved). Por eso
los tests de `tests/test_sign_firmware.py` releen los textos de rechazo del
propio `.cpp`.

---

## 1. Dependencias

Hace falta una libreria Ed25519. Se prefiere `cryptography`:

```bash
pip install cryptography
```

Si no esta, PyNaCl tambien sirve (y entonces las claves se guardan en formato
crudo con base64 en vez de PEM):

```bash
pip install pynacl
```

Sin ninguna de las dos, la herramienta **no firma ni verifica** y sale con
codigo 2. Es deliberado: es el mismo fail-closed que el firmware cuando no
tiene `CryptoBackend` (docs/ota_procedure.md §10). Un supervisor de seguridad
que acepta firmware sin comprobarlo es peor que uno que no acepta firmware.

---

## 2. Generar las claves

```bash
python3 tools/sign_firmware.py keygen --out-prefix keys/ota
```

Deja dos ficheros:

| Fichero | Contenido | Permisos |
| --- | --- | --- |
| `keys/ota_private.key` | Clave privada Ed25519 (PEM PKCS#8) | `0600` |
| `keys/ota_public.key` | Clave publica Ed25519 (PEM) | `0644` |

El comando imprime ademas la clave publica en hexadecimal, que es lo que se
compila en `mcu/include/ota_pubkey.h`:

```text
pubkey publica en hex, para mcu/include/ota_pubkey.h:
  6298fc7266de96378491d657f1dbaa0e7943883f57fb347e5e7fb099bcb19ef2
```

`keygen` no sobrescribe nada salvo que se le pase `--force`.

---

## 3. Firmar

```bash
python3 tools/sign_firmware.py sign build/supervisor.bin \
    --version 12 \
    --build-id "$(git rev-parse --short HEAD)" \
    --key "$OTA_SIGNING_KEY" \
    --out supervisor-v12.fwim
```

| Opcion | Que hace |
| --- | --- |
| `--version N` | Version monotonica de la imagen (obligatorio). Nunca decrece. |
| `--build-id N` | Git short sha o timestamp Unix. Por defecto 0. |
| `--key CLAVE` | Clave privada, en PEM o en el formato crudo propio. |
| `--out IMAGEN` | Fichero `.fwim` de salida. |
| `--slot-max N` | Slot de flash usado en la autocomprobacion. Por defecto 524288. |

`--build-id` acepta un entero. Para meter un SHA hay que convertirlo
(`git rev-parse --short HEAD` da hex):

```bash
python3 tools/sign_firmware.py sign build/supervisor.bin \
    --version 12 \
    --build-id $((16#$(git rev-parse --short HEAD))) \
    --key "$OTA_SIGNING_KEY" \
    --out supervisor-v12.fwim
```

Despues de escribir, el comando **se autoverifica** con la clave publica que
ha derivado de la privada. Si lo que acaba de escribir no pasara el pipeline,
falla con codigo 1 en vez de dejar un `.fwim` inservible en el disco.

---

## 4. Verificar

```bash
python3 tools/sign_firmware.py verify supervisor-v12.fwim \
    --pubkey keys/ota_public.key
```

Salida correcta (codigo 0):

```text
OK supervisor-v12.fwim: version 12, build_id 0xDEADBEEF, payload 8192 bytes, total 8320
```

Salida con rechazo (codigo 1):

```text
RECHAZADA supervisor-v12.fwim: firma invalida (SIGNATURE_INVALID, paso 8 de 9)
```

| Opcion | Que hace |
| --- | --- |
| `--pubkey CLAVE` | Clave publica: PEM, hex de 32 bytes o base64. |
| `--slot-max N` | Slot de flash. Por defecto 524288. |
| `--min-version N` | Version ya instalada. La imagen debe ser **estrictamente** mayor. Por defecto 0. |
| `--json` | Veredicto como JSON, para CI. |

Con `--json` la salida es un unico objeto, tanto si acepta como si rechaza:

```json
{"reject": "OK", "reject_code": "NONE", "version": 12, "payload_len": 8192,
 "build_id": 3735928559, "total_len": 8320, "steps_done": 9, "ok": true}
```

`reject` lleva el mismo texto que devuelve `fwRejectText()` en el firmware, y
`reject_code` su nombre de enumerado. `steps_done` dice hasta que paso llego:
`9` es una imagen aceptada, `7` significa que no se llego a comprobar la
firma.

---

## 5. Formato

Todo little-endian. El manifiesto son 64 bytes fijos:

| Offset | Tam | Campo | Notas |
| --- | --- | --- | --- |
| `0x00` | 4 | `magic` | `0x4D495746` = bytes `FWIM` |
| `0x04` | 4 | `header_len` | Siempre 64 |
| `0x08` | 4 | `version` | Monotonica |
| `0x0C` | 4 | `payload_len` | Bytes de firmware |
| `0x10` | 4 | `build_id` | Short sha o timestamp |
| `0x14` | 4 | `payload_crc32` | CRC-32/ISO-HDLC del payload |
| `0x18` | 32 | `payload_sha256` | SHA-256 del payload |
| `0x38` | 4 | `header_crc32` | CRC-32 de `[0x00, 0x38)` |
| `0x3C` | 4 | `reserved` | 0 |
| `0x40` | N | `payload` | El binario |
| `0x40+N` | 64 | `signature` | Ed25519 de `[0x00, 64+N)` |

### Orden de armado (lo que se firma)

El manifiesto se rellena en **dos fases**, porque `header_crc32` esta dentro
del propio manifiesto:

1. Se escriben los bytes `[0x00, 0x38)`.
2. `header_crc32 = crc32` de esos 56 bytes, y se escribe en `0x38`.
3. `reserved = 0` en `0x3C`. Ya hay un manifiesto completo de 64 bytes.
4. La firma se calcula sobre `manifiesto_completo || payload`, es decir los
   primeros `64 + payload_len` bytes. **Incluye** `header_crc32` y `reserved`.
5. La firma de 64 bytes se anexa al final.

El paso 4 incluye el CRC del manifiesto porque el firmware verifica la firma
sobre `image[0 .. header_len + payload_len)`. Si se firmara solo el payload, o
si se firmara antes de escribir el CRC, la imagen seria rechazada.

---

## 6. Orden de verificacion

El mismo que `fwVerifyImage()`, y por el mismo motivo:

```text
1. magic
2. header_len
3. payload_len cabe en el slot
4. longitud total sin overflow ni truncamiento
5. version estrictamente mayor que la instalada
6. CRC-32 del manifiesto
7. CRC-32 del payload
8. SHA-256 del payload
9. firma Ed25519
```

Los pasos 1-5 **solo rechazan**: no aceptan nada en funcion de datos sin
verificar, unicamente evitan que una imagen manipulada provoque un
desbordamiento de buffer. Por eso pueden ir antes de la firma sin abrir un
agujero. La firma va al final porque es lo mas caro.

El texto de rechazo es exactamente el de `fwRejectText()`:

| Codigo | Texto |
| --- | --- |
| `NONE` | `OK` |
| `TOO_SMALL` | `imagen menor que el manifiesto` |
| `BAD_MAGIC` | `magic invalido` |
| `BAD_HEADER_LEN` | `tamano de manifiesto inesperado` |
| `PAYLOAD_TOO_BIG` | `payload no cabe en el slot` |
| `IMAGE_TRUNCATED` | `imagen truncada` |
| `IMAGE_TOO_BIG` | `tamano total desbordado` |
| `VERSION_NOT_NEWER` | `version no mas nueva` |
| `HEADER_CRC_FAIL` | `CRC de manifiesto incorrecto` |
| `PAYLOAD_CRC_FAIL` | `CRC de payload incorrecto` |
| `PAYLOAD_HASH_FAIL` | `SHA-256 de payload incorrecto` |
| `NO_CRYPTO_BACKEND` | `sin backend criptografico` |
| `SIGNATURE_INVALID` | `firma invalida` |

---

## 7. Uso en CI

El job `firmware-sign` de `.github/workflows/ci.yml` hace el ciclo completo con
una clave efimera: genera, firma, verifica que la imagen buena pasa, manipula
un byte y **comprueba que el fallo se detecta**, y lanza los tests. El job
pasa solo si la deteccion funciona; si una manipulacion pasara desapercibida,
el job fallaria.

El `verify --json` permite encadenar el resultado en pasos posteriores:

```yaml
- name: Verificar firma del firmware
  run: |
    python3 tools/sign_firmware.py verify build/supervisor.fwim \
      --pubkey keys/ota_public.key --json
```

Un artefacto sin firma verificable no se publica.

---

## 8. Gestion de claves

**La clave privada NUNCA entra en git.** Ni en el repo, ni en un `.env`
commiteado, ni en la planta, ni en un artefacto de CI.

```text
CI secret  ->  firma la imagen  ->  publica el artefacto firmado
```

| Donde | Que |
| --- | --- |
| `keys/ota_private.key` en local | Solo para pruebas. **Nunca commitear.** |
| Secret de CI (`OTA_SIGNING_KEY`) | La que firma de verdad. |
| `mcu/include/ota_pubkey.h` | La publica, compilada en el firmware. |

Como la clave publica va compilada dentro del firmware, una actualizacion
**no puede cambiarla**: solo el bootloader o ST-Link, y ambos son fisicos. Es
deliberado, si el firmware pudiera cambiar su propia clave publica la firma
dejaria de proteger nada.

Si la clave privada se pierde, se genera otra y se reprograma por ST-Link.
Las imagenes firmadas con la clave vieja dejan de verificar: es el fallo
esperado, no un bug.

Lo que **si** se commitea es la clave publica, y `tools/` no genera nada
dentro del repo salvo que se le pase un `--out-prefix` explicito. Para pruebas
locales, usa un directorio fuera del arbol versionado:

```bash
python3 tools/sign_firmware.py keygen --out-prefix /tmp/ota-pruebas/ota
```

---

## 9. Tests

```bash
./test_venv/bin/pytest tests/test_sign_firmware.py -v
```

Cubren el round-trip por CLI y, sobre todo, cada rechazo del pipeline. Si no
hay `cryptography` ni `pynacl` los tests se saltan enteros, para no romper
`make tests` en una maquina sin dependencias.
