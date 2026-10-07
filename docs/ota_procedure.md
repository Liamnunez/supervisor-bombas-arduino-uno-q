# Procedimiento de Actualización Firmware (OTA Firmado) - Fase 4

> Cómo se actualiza el firmware sin convertir al supervisor en una puerta
> abierta, y por qué la activación es **local** aunque la descarga sea remota.

---

> ⚠️ **DOCUMENTO NO APROBADO.** El diseño de particiones A/B depende de
> verificar el mapa de flash real del UNO Q y la disponibilidad de
> dual-bank en el STM32U585. Nada de la parte de hardware está validado.

---

## 1. El problema real

Un firmware de seguridad que se puede reemplazar en remoto es un firmware
que se puede *desactivar* en remoto. La regla del proyecto es que el límite
de 1 bomba en GENERADOR no se puede sobrescribir con ningún comando. Una
actualización remota que escriba el firmware es, técnicamente, un comando
con capacidad de reescribir esa regla.

Esto no significa "no actualizar". Significa que la actualización necesita
**más controles que una descarga autenticada**.

---

## 2. Qué protege cada mecanismo

| Amenaza | Mecanismo que la frena |
| --- | --- |
| Imagen corrupta por red / bit flip | CRC32 + SHA-256 |
| Imagen manipulada en tránsito | Firma Ed25519 |
| Firmware firmado pero con **fallo** | Auto-test de arranque + rollback A/B |
| Binario antiguo reintroducido (downgrade) | Versión monotónica + anti-rollback |
| Alguien con acceso a la red **legítimo** (admin) cambia la lógica | Activación local obligatoria |
| Firmware firmado pero compilado de un repo comprometido | Verificación de firma en CI, clave fuera del repo |

> **La firma sola no protege de un firmware malo.** Una firma válida
> demuestra *autenticidad*, no *corrección*. Un developercommitted con la
> clave puede firmar firmware peligroso. De ahí el auto-test y el rollback.

---

## 3. Arquitectura escalonada (3 pasos)

No todo el riesgo es igual en cada etapa. Por eso el proyecto lo hace por
niveles, empezando por el que captura más seguridad con menos riesgo.

### Paso 1 — Firma verificada en CI, flasheo físico ✅ menor riesgo

```text
CI:  compila → extrae binario → verifica firma Ed25519 con la clave pública
     → publica artefacto firmado (firma + hash en el tag de release)

Operador:  descarga artefacto → verifica hash → flashea por ST-Link (en planta)
```

| Ganancia | Costo |
| --- | --- |
| El binario que llega a la planta es el que se compiló y firmó | Viaje técnico por corrección |
| Cero ruta de ejecución remota | — |
| Cero cambios en el bootloader | — |
| La clave privada nunca sale del entorno de CI | — |

> **Este paso ya cubre la amenaza principal** (binario troyano en la planta)
> sin tocar el hardware. Antes de implementar el paso 2 o 3, hay que
> comprobar si el paso 1 basta para el caso de uso real.

### Paso 2 — Descarga remota verificada, activación local ⚠️ riesgo medio

```text
Linux:   descarga imagen → la envía al MCU → el MCU verifica firma + hash
MCU:     la guarda en slot B → reporta PENDIENTE → NO activa
Operador: gira llave física / pulsa el botón de servicio → permite el reinicio
MCU:     arranca en slot B → se auto-testea → CONFIRMA o hace ROLLBACK
```

| Ganancia | Costo |
| --- | --- |
| Sin viaje técnico: el binario ya está en la planta | Hay que montar A/B en flash |
| Un firmware roto se revierte solo | Un auto-test fallido deja la bomba parada hasta reiniciar |
| Cambios de firmware siguen siendo decisiones locales | La gestión de slots es nueva y debe probarse |

### Paso 3 — Activación remota con 2FA ⚠️⚠️ riesgo alto — **NO RECOMENDADO**

Permitir que un admin active remotamente un firmware nuevo.

**Por qué no se recomienda** (aun con firma válida):

- La validación de firmware pasa de "probada en planta" a "probada por
  quien la firmó".
- Un token de admin comprometido pasa de "puede resetear una emergencia" a
  "puede cambiar el firmware que decide qué es una emergencia".
- Los proof tests PT-01..PT-09 se ejecutan contra un firmware que cambió
  sin que nadie lo haya revisado.

> Si alguna vez hace falta, el control que lo hace aceptable es que la
> **clave de firma de producción solo puede usarse en CI**, y el CI exige
> aprobación humana y re-ejecuta PT-01..PT-07 automatizados antes de
> publicar. Eso ya es un proceso de gestión de cambios, no una función del
> firmware.

---

## 4. Modelo de la imagen de firmware

El manifiesto va al principio de la imagen. Todo offsets en bytes.

```text
Offset  Tam  Campo           Descripción
------  ---  --------------  ------------------------------------------
0x00     4   magic           0x4D495746 LE = bytes "FWIM"
0x04     4   header_len      Tamaño del manifiesto (= 64)
0x08     4   version         Versión monotónica (nunca decrece)
0x0C     4   payload_len     Bytes de firmware
0x10     4   build_id        Git short SHA o timestamp Unix
0x14     4   payload_crc32   CRC32 del payload
0x18    32   payload_sha256  SHA-256 del payload
0x38     4   header_crc32    CRC32 de los bytes 0x00..0x37
0x3C     4   reserved        0
----- fin manifiesto (64 bytes) -----
0x40     N   payload         Binario del firmware
0x40+N  64   signature       Ed25519 sobre (0x00..0x3B || payload)
```

### Orden de verificación (importante)

```text
1. magic           barato, rechaza temprano
2. header_len      longitud fija esperada
3. payload_len     ¿cabe en el slot de flash?
4. longitud total  ¿overflow? (bounds, no confianza)
5. version         monotónica + anti-rollback
6. CRC32 + SHA-256 del payload vs manifiesto
7. FIRMA Ed25519   ← la más cara, la última
```

> **Por qué los bounds antes de la firma es seguro:** los pasos 1-5 solo
> *rechazan*. No aceptan nada en función de datos no verificados; solo
> evitan que una imagen manipulada provoque un desbordamiento de buffer.
> Nada del manifiesto se *usa* hasta después de la firma.
>
> **Por qué la firma al final:** la verificación Ed25519 es la más cara
> (decenas de miles de operaciones). Rechazar primero lo obviamente inválido
> evita pagar esa factura con basura de red.

---

## 5. Máquina de estados de los slots

```text
        ┌──────────────┐
        │   EMPTY      │  slot sin usar
        └──────┬───────┘
               │ imagen verificada y guardada
               ▼
        ┌──────────────┐
        │  PENDING     │  guardada, NO activada
        └──────┬───────┘
               │ arranque local confirmado (llave/botón de servicio)
               ▼
        ┌──────────────┐
        │  TRIAL       │  arrancando desde este slot
        └──────┬───────┘
               │            ┌──────────────────┐
     auto-test │            │ auto-test falló  │
     OK        │            │ o no confirmó    │
               ▼            ▼
        ┌──────────────┐  ┌──────────────┐
        │  CONFIRMED   │  │  FAILED      │──┐
        └──────────────┘  └──────────────┘  │ rollback
                                         └──► vuelve al slot anterior
```

### El auto-test de arranque

El firmware que arranca en modo `TRIAL` **no** se da por bueno hasta que
compruebe lo mínimo que lo hace útil:

| # | Prueba | Si falla |
| --- | --- | --- |
| 1 | `SelfTest::relaysOffAtBoot()` — los relés nacen abiertos | Rollback |
| 2 | `SelfTest::heartbeatGenerating()` — el latido PNOZ fluye | Rollback |
| 3 | `SelfTest::stateMachineReachable()` — la máquina responde | Rollback |
| 4 | `SelfTest::protectionConfigured()` — umbrales > 0 y coherentes | Rollback |
| 5 | `SelfTest::protocolCrcOk()` — CRC del protocolo | Rollback |

> **El auto-test nunca prueba el relé de seguridad.** Eso es hardware. Un
> firmware que no arranca el latido es la prueba de que el PNOZ hace su
> trabajo: las bombas se paran. Eso es el resultado correcto.

### Tiempo de confirmación

Si el firmware en `TRIAL` no confirma en `OTA_CONFIRM_TIMEOUT_MS`, el
watchdog/arranque revierte al slot anterior. Un firmware que se cuelga
antes de confirmar **nunca** queda activo.

---

## 6. Por qué la activación es local

El razonamiento completo está en §3. En corto:

- La **descarga** remota no cambia el comportamiento: una imagen no
  activada es un archivo inerte.
- La **activación** cambia el comportamiento de una función de seguridad.
- La firma garantiza que la imagen es auténtica; no garantiza que sea
  correcta. Solo una persona en la planta, ejecutando los proof tests,
  puede establecer eso.

---

## 7. Claves

| Clave | Dónde | Quién la tiene |
| --- | --- | --- |
| Ed25519 **privada** (firmar) | Solo en CI, como secret | El que controla el pipeline |
| Ed25519 **pública** (verificar) | En el firmware (`ota_pubkey.h`) | Todos |
| Clave de rollback | N/A | La verificación de rollback va en el bootloader, no en el firmware |

> La clave pública **va compilada dentro del firmware**. Eso significa que
> una actualización **no puede cambiar la clave pública**: solo el bootloader
> o ST-Link pueden hacerlo, y ambos son accesibles físicamente. Es
> deliberado: si el firmware pudiera cambiar su propia clave pública, la
> firma dejaría de proteger nada.

### La clave nunca entra en git

```text
CI secret  →  firma la imagen  →  publica artefacto
```

La clave privada no está en el repo, ni en `.env`, ni en la planta.
Si se pierde, se genera una nueva y se reprograma por ST-Link.

---

## 8. Firmar (herramienta de referencia)

```bash
# En el entorno de CI, nunca en la planta
pip install pynacl

python3 -m tools.sign_firmware \
    build/supervisor.elf \
    --version 12 \
    --key "$OTA_SIGNING_KEY" \
    --out supervisor-v12.fwim

# Verificar sin la clave privada
python3 -m tools.sign_firmware --verify supervisor-v12.fwim --pubkey ota_pubkey.der
```

> El script real es `tools/sign_firmware.py`. **No existe todavía** — ver
> §10. Especifica el formato, no la implementación.

---

## 9. Verificación en CI

Añadir al pipeline antes de publicar cualquier artefacto:

```yaml
- name: Verificar firma del firmware
  run: |
    python3 tools/sign_firmware.py --verify build/supervisor.fwim \
      --pubkey keys/ota_pubkey.der
```

Si la verificación falla, el job falla. Un artefacto sin firma verificable
no se publica.

---

## 10. Qué está y qué no está implementado

| Pieza | Estado |
| --- | --- |
| `mcu/include/fw_image.h/.cpp` — formato y pipeline de verificación | ✅ Implementado y testeado |
| `mcu/include/boot_slot.h/.cpp` — máquina de estados A/B y rollback | ✅ Implementado y testeado |
| `mcu/include/self_test.h/.cpp` — auto-test de arranque | ✅ Implementado y testeado |
| Interfaz `CryptoBackend` para Ed25519 | ✅ Interfaz + doble de test |
| **Implementación real de Ed25519** | ❌ **Pendiente — librería externa** |
| Partición A/B en flash | ❌ Pendiente de hardware |
| bootloader que respecta los slots | ❌ Pendiente |
| `tools/sign_firmware.py` | ❌ Pendiente |
| Paso 1 (firma en CI + flasheo físico) | ❌ Pendiente — **el de mejor relación riesgo/valor** |

### Sobre la implementación de Ed25519

**No se implementa criptografía a mano.** Se usa una librería auditada
(monocypher, ~2.5k líneas, dominio público,常数-time) fijada por hash de
commit. La interfaz `CryptoBackend` existe para que:

1. El resto del pipeline sea testeable nativamente sin criptografía real.
2. Sea explícito dónde entra la dependencia externa.
3. Cambiar de librería no toque la lógica de seguridad.

> Mientras `CryptoBackend` no tenga una implementación real, **toda imagen
> debe rechazarse**. El comportamiento por defecto (fail-closed) es correcto:
> un supervisor de seguridad que acepte firmware sin verificar es peor que
> uno que no acepte firmware.

---

## 11. Proof tests de la actualización

Añadir a `docs/hardware_spec.md` cuando exista bootloader:

| # | Prueba | Criterio |
| --- | --- | --- |
| **PT-10** | Imagen con firma corrupta | Rechazada, slot actual intacto |
| **PT-11** | Imagen con hash que no cuadra | Rechazada |
| **PT-12** | Imagen versión anterior | Rechazada (anti-rollback) |
| **PT-13** | Firmware con auto-test que falla | Rollback automático |
| **PT-14** | Corte de corriente durante transferencia | Slot destino no se corrompe |
| **PT-15** | Corte de corriente durante activación | Arranca en el slot confirmado |

> **PT-14 y PT-15 son las críticas.** Un corte de luz a media escritura es
> el escenario real, no el teórico. Si el bootloader no sobrevive a eso, no
> hay A/B que valga.

---

## 12. Referencias

| Documento | Para qué |
| --- | --- |
| `docs/security_protocols.md` | SIF, PT-01..PT-09, RBAC |
| `docs/hardware_spec.md` | Particiones de flash (pendiente), PNOZ |
| `docs/playbooks.md` | PB-05 (MCU caído) interactúa con el rollback |
| `docs/incident_response.md` | Un firmware nuevo es un cambio que hay que auditar |

---

Fase 4 — Documento 1 de 1
