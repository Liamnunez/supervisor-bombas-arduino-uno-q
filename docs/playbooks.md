# Playbooks Operativos - Fase 5

> Pasos concretos para cada escenario. **El sistema es autónomo**: la mayoría
> de estos escenarios no requieren acción de ninguna persona.

---

> ⚠️ **DOCUMENTO NO APROBADO.** Números de umbral y ventanas deben validarse
> contra las placas de datos reales antes de usarlo en operación.

---

## Resumen: qué requiere persona y qué no

| # | Escenario | Persona | Acción |
| --- | --- | --- | --- |
| PB-01 | Tormenta + generador | No | Automático: 1 bomba |
| PB-02 | Pozo sube en generador | No | Automático: bloquea 2.ª |
| PB-03 | Trip por sobrecorriente | No* | Automático: abre y re-intenta |
| PB-04 | Contactor pegado | **Sí** | Verificar MCC |
| PB-05 | MCU caído / PNOZ abre | **Sí** | Reiniciar MCU |
| PB-06 | E-Stop físico | **Sí** | Investigar + autorizar retorno |
| PB-07 | Sin telemetría | No | Automático: reintenta |

\* PB-03 solo requiere persona si se agotan los 3 reintentos.

---

## PB-01 — Tormenta con generador (sin acción)

**Disparador:** contacto ATS a GENERADOR durante una tormenta.

**Qué hace el sistema:**

1. Detecta modo GENERADOR (contacto abierto → LOW).
2. Transiciona a `GENERADOR` (`MODO_CHANGE` 0x40 a Linux).
3. SIF-01: `puedeArrancar()` permite **solo bomba 1**.
4. Si el PLC ordena B2 o B3, se **bloquea** el arranque y se registra.
5. B1 corre (~30 A). El generador queda por debajo de su límite.

**Qué ve la persona:** un WARNING informativo en el dashboard. Nada más.

**Qué hacer:** nada. Es el diseño normal.

**Qué NO hacer:** no forzar el arranque de B2/B3 "para achicar el pozo".
Eso es exactamente lo que dispara el apagón del DSE.

---

## PB-02 — El pozo sube y solo hay una bomba disponible

**Disparador:** nivel > 90 % con el sistema en GENERADOR.

**Realidad:** con 30 A de capacidad y bombas de 30 A, **solo cabe una**.
Si el pozo sube, es porque la capacidad de extracción no alcanza al caudal
de entrada. Ninguna électronique resuelve eso.

**Qué hace el sistema:**

1. Alerta `NIVEL_CRITICO_ALTO` (> 90 %).
2. Mantiene B1 corriendo (no para la bomba — para el pozo).
3. Si el PLC ordena más bombas, se bloquean (SIF-01).

**Qué debe hacer la persona (esta sí es una decisión humana):**

| Opción | Cuándo | Riesgo |
| --- | --- | --- |
| **A. Subir el umbral de nivel** | Nivel alto sostenido > 4 h | Real: el pozo puede desbordar |
| B. Conectar la red (salir de generador) | Si hay red disponible | Depende del DSE |
| C. Aceptar y esperar | Nivel < 95 % | Bajo, pero sube |
| D. Parar y no bombear | Nivel > 95 % | Alto: desbordamiento |

> **El sistema no elige entre estas.** Puede, y debería, avisar — pero la
> decisión de "dejar de proteger el generador para proteger el pozo" es
> una decisión de operación que pertenece a una persona con contexto del
> lugar. Está documentada en `docs/hazop_lopa.md` como riesgo residual.

**Decisión tomada por el usuario:** proteger el generador. Una bomba antes
que cero bombas.

---

## PB-03 — Trip por sobrecorriente (auto-recuperación)

**Disparador:** corriente > 42 A sostenida 3 s (SIF-04).

**Qué hace el sistema (implementado en `mcu/src/trip_policy.cpp`):**

1. Abre **todos** los relés → corriente a 0.
2. El DSE ve una caída de carga, no una sobrecarga → no corta.
3. Espera el enfriamiento (`TRIP_COOLDOWN_MS` = 60 s).
4. Si la corriente bajó de 5 A, re-arma solo y vuelve al estado anterior.
5. Repite hasta **3 intentos en 15 minutos**.
6. Al agotarlos: `EMERGENCIA` latched + `ERR_TRIP_LATCH` (0x6021).
   **Ahí sí requiere reset.**

**Por qué abrir todo y no solo una bomba:** si la corriente es demasiado
alta, dejar cualquier bomba andando mantiene al generador en sobrecarga.
El DSE corta, y entonces quedan **0 bombas** y un generador en mal estado.
Abrir todo cuesta 1 bomba ahora; no abrirlo cuesta las 3 después.

**Por qué 60 s de enfriamiento:** para no machacar los motores con arranques
repetidos. La protección del generador ya la hizo el paso 1 (abrir relés);
esperar no añade seguridad, solo evita 3 arranques en 2 minutos.

**Qué ve la persona:**

| Intentos | Códigos | Alerta | Acción |
| --- | --- | --- | --- |
| 1.º | 0x6001 luego 0x6020 | info | Ninguna |
| 2.º | 0x6001 luego 0x6020 | info | Ninguna |
| 3.º | 0x6001 y 0x6021 | **CRITICAL** | **Ir a resetear** |

> **Patrón a vigilar en el dashboard:** un 0x6001 seguido de 0x6020 es un
> trip recuperado solo. Un 0x6001 **sin** 0x6020 posterior significa que el
> sistema no se recuperó y hay que mirarlo.

**Reset (solo R2/R3, con token `operator` o `admin`):**

```bash
# Rota dedicada. Acepta el "motivo" y lo escribe en el audit log JSONL,
# que es lo que permite reconstruir la intervencion meses despues.
curl -X POST https://<host>/api/mcu/reset_emergencia \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"motivo":"trip x3 - revisar CT y contactores"}'
```

> El campo `motivo` no es obligatorio para que el comando funcione (puede ser
> una parada rápida), pero **siempre queda registrado**, incluso vacío.
>
> Alternativa genérica: `POST /api/command` con
> `{"command":"reset_emergencia","motivo":"..."}`.

---
> El `motivo` queda en el audit log JSONL y es lo que permite reconstruir
> qué pasó tres meses después.
>
> El reset del operador **también pone a cero los intentos** de la política
> de trip (`CommBridge::handleCommand` → `policy.operatorReset()`). Sin eso, el
> siguiente trip heredaría los intentos viejos y podría latchear de
> inmediato, con un solo fallo nuevo.

**Lo que la auto-recuperación NO limpia:** los faults de feedback de cada
bomba. Si una bomba perdió el retorno auxiliar, sigue bloqueada tras el
auto-re-arm. Solo el reset de operador los libera (`autoRecoverTrip()` no
llama a `clearFaults()`; `resetEmergencia()` sí).

---

## PB-04 — Contactor pegado (acción de persona)

**Disparador:** corriente > 2 A con **todos** los relés abiertos.

**Qué significa:** un contactor está soldado, o hay una carga fuera del
supervisor, o el CT está mintiendo.

**Qué hace el sistema:**

1. Detecta `CONTACTOR_WELDED` (ya viene con los relés abiertos: la
   detección es "corriente fluyendo sin nuestro permiso").
2. Envía alerta crítica + telemetría. No toca los relés.

**Por qué no intenta hacer nada más:** si el contactor está soldado, el
relé del MCU ya no manda sobre esa bomba. Abrir o cerrar relés es
indiferente; lo único que puede hacer es avisar. Por eso este escenario
**siempre** necesita a una persona y no tiene auto-recuperación.

**Pasos para R4 (eléctrico):**

1. Confirmar en el panel que aparece la alarma.
2. **No** pulsar E-Stop — no es una emergencia de persona.
3. Cortar el seccionador del circuito afectado.
4. Verificar el contactor pegado (inspección visual + ohm).
5. Reparar o sustituir.
6. Verificar que el auxiliar NO (PC1/PC2/PC3) refleja el estado real.
7. Resetear emergencia desde el panel.
8. Registrar en el acta de intervención.

> ⚠️ **No intervenir dentro del MCC energized.** Procedimiento en
> `docs/wiring.md` y normativa de la planta.

---

## PB-05 — MCU caído (acción de persona)

**Disparador:** el PNOZ s4 deja de ver el latido y abre sus salidas.

**Qué significa:** el firmware se colgó, se quedó sin alimentación, o el
cable del latido se cortó. **El sistema se paró solo, sin necesitar a
nadie.** Eso es el diseño correcto.

**Pasos para R2/R3:**

1. Verificar la alerta en Telegram.
2. Verificar alimentación 24 V en el bornero del MCU.
3. Verificar LED del PNOZ s4 (¿verde = pulsos OK?).
4. **Comprobar el cable de latido PB3 → PNOZ.** Si el PNOZ está OK y el
   cable está mal, es el cable. Si el PNOZ no está OK, es el MCU.
5. Reiniciar el MCU (ciclo de alimentación, **no** reflashear).
6. Al arrancar, el firmware deja el pin de latido en LOW y espera al
   primer pulso → el PNOZ se rearma solo.
7. Confirmar que las bombas arrancan según lo ordene el PLC.
8. Si el PNOZ **vuelve** a abrir, el problema no es transitorio: buscar
   la causa (watchdog, brownout, cortocircuito en el latido).

**Por qué no se auto-recupera:** el MCU no puede reiniciarse a sí mismo
cuando está colgado. Es el límite físico del diseño.

---

## PB-06 — E-Stop físico (acción de persona, por definición)

**Disparador:** alguien presionó el hongo, o hay un fallo en los dos canales.

**Por qué esta es especial:** un E-Stop significa que **una persona
corrió peligro**. No es un fallo del sistema.

**Pasos inmediatos:**

1. El PNOZ ya abrió los relés. **No hacer nada todavía.**
2. Ir a la ubicación del botón.
3. Verificar quién lo activó y por qué.
4. **No** desenclavar hasta entender la causa.
5. Verificar que no hay nadie en riesgo.
6. Desenclavar el botón (giro + tirón).
7. Resetear el PNOZ s4 (`Y1-Y2`).

**Antes de devolver a servicio (R5 autoriza):**

- [ ] Causa identificada y registrada
- [ ] Área despejada
- [ ] Reinicio de bombas verificado
- [ ] Nadie en riesgo
- [ ] Acta de incidente firmada

> **R5 (seguridad) autoriza el retorno.** No es un tecnicismo: si el
> E-Stop se activó por un atrapamiento, volver a arrancar sin
> confirmar el área es cómo se repite el accidente.

**Criterio de "no fue una persona":** si el PNOZ reporta apertura sin que
nadie presionara el botón, es un fallo de los canales o un cortocircuito.
Se trata como PB-05 (MCU/circuito) pero se escala a R4 y R5.

---

## PB-07 — Sin telemetría (sin acción)

**Disparador:** no llega heartbeat del MCU durante > 12 s.

**Qué hace el sistema:** el hilo de lectura reabre el puerto serie con
backoff creciente (1 s → 30 s) y, al recuperar, pide el estado al MCU. Antes
solo capturaba la excepción y seguía leyendo un puerto cerrado para siempre.

El MCU sigue funcionando independientemente de que Linux esté o no: la
seguridad no depende de la telemetría.

**Este es el principio de la arquitectura:** la seguridad no depende de la
telemetría. Si cae el servidor, las bombas siguen bajo el control del
firmware.

**Cuándo sí requiere persona:**

| Situación | Severidad | Acción |
| --- | --- | --- |
| > 5 min sin telemetría | CRITICAL (C-08) | Verificar red / proceso |
| > 30 min sin telemetría | CRITICAL | R2 presencial |

---

Fase 5 — Documento 3 de 5
