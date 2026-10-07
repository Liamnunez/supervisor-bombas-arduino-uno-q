# Manual del Operador - Fase 6

> Lo que necesita saber la persona que está frente al panel. Ni más ni
> menos: todo lo que hay que saber está aquí, y todo lo que hay aquí se
> puede hacer.

---
> ⚠️ **Placeholders:** los nombres, turnos y contactos marcados `PENDIENTE`
> deben completarse antes de entregar este manual a nadie.

---

## 1. Lo primero, y lo más importante

**El supervisor es automático. En operación normal no haces nada.**

No hay que vigilar nada, no hay que ajustar nada, no hay que tocar nada.
Tu trabajo aparece cuando:

| Situación | Qué haces |
| --- | --- |
| **Arranque** | Poner en marcha el sistema |
| **Parada** | Parada programada o mantenimiento |
| **Reset** | Un evento terminó los 3 intentos y exige tu presencia |

Cualquier otra cosa la resuelve el firmware. Si estás pensando en tocar algo
porque "algo va raro", casi siempre la respuesta correcta es **no tocar**.

> **La razón por la que esto importa:** si tocas el sistema cuando no debes,
> estás anulando la protección que existe para que las bombas no se queden
> sin protección. Un sistema que se interviene sin necesidad termina sin
> protección en el momento en que más falta hace.

---

## 2. Qué hace el sistema por ti

```text
Modo RED (normal)      → 3 bombas disponibles, el PLC decide
Modo GENERADOR         → solo la bomba 1, automáticamente
Tormenta               → nada, ya está cubierto
Nivel sube             → nada, ya está cubierto
Sobrecarga             → para todo y re-intenta solo (3 veces)
```

La regla de fondo: **el generador no soporta dos bombas.** Por eso, en
generador, solo puede andar una. Eso lo decide el equipo, no tú, y no hay
ningún botón para cambiarlo.

---

## 3. Los cuatro estados

| Estado | Qué significa | Bombas | ¿Quién lo ve? |
| --- | --- | --- | --- |
| **NORMAL** | Red eléctrica, todo normal | Las 3 disponibles | LED verde |
| **GENERADOR** | Generador activo | Solo la bomba 1 | LED amarillo |
| **EMERGENCIA** | Parada por protección | 0, exige reset | LED rojo parpadeando |
| **MANTENIMIENTO** | Parada localizada por una persona | 0 | LED verde parpadeando |

---

## 4. Los tres botones (y por qué están restringidos)

| Acción | Token | Qué hace | Cuándo |
| --- | --- | --- | --- |
| **Reset emergencia** | `operator` o `admin` | Re-arma tras 3 trips fallidos | Solo cuando el LED rojo parpadea |
| **Mantenimiento on/off** | `operator` o `admin` | Para las bombas de forma deliberada | Trabajo en el equipo |
| **Modo generador** | Solo `admin` | Fuerza el modo sin el contacto físico | Excepcional |
| **Forzar emergencia** | Solo `admin` | Para las bombas a propósito | Emergencia real |

> **Por qué "modo generador" es de admin y no de operator:** ese comando
> cambia el criterio de protección. Con `admin`, una sola bomba; sin él, las
> tres. Quien lo ejecuta decide cuánta protección hay. Por eso está
> separado del resto.

---
> ⚠️ **Aviso:** cambiar el modo generador a distancia **desactiva el
> interlock de 1 bomba**. Es la excepción documentada al principio del
> proyecto y solo se usa cuando hay un motivo real.

---

## 5. Procedimientos que sí te corresponden

### 5.1 Reset de emergencia (lo más frecuente)

**Cuándo:** el LED rojo parpadea y Telegram dice "intentos agotados".

1. Abre el dashboard.
2. Mira **por qué** se agotaron los intentos. No hagas el reset a ciegas.
3. Revisa estas tres cosas:
   - ¿Hay carga extra en el generador que no debería estar?
   - ¿Alguno de los contactores está pegado?
   - ¿El sensor de corriente está bien conectado?
4. Si el motivo está claro y es transitorio → **reset**.
5. Si no está claro → **no resetees**, avisa a R4 (eléctrico).

```bash
curl -X POST https://<host>/api/mcu/reset_emergencia \
  -H "Authorization: Bearer <token>" \
  -H "Content-Type: application/json" \
  -d '{"motivo":"trip x3 - revisar CT y contactores"}'
```

> **El campo `motivo` es lo más importante del procedimiento.** Lo que no
> se escribe, tres meses después no existe.

### 5.2 Parada de mantenimiento

1. Avisa a R2 (tu supervisor) **antes** de parar.
2. Comprueba que no haya riesgo de desbordamiento del pozo.
3. Activa mantenimiento.
4. Haz el trabajo.
5. Al terminar: desactiva mantenimiento y verifica que las bombas arrancan.

### 5.3 Arranque tras parada larga

1. Verifica alimentación 24 V.
2. Verifica que el LED del PNOZ está en verde.
3. Enciende el sistema.
4. Espera: el arranque no ordena ninguna bomba por su cuenta. Las arranca
   el PLC cuando el nivel lo pide. **Esto no es un fallo.**
5. Confirma en el dashboard que hay comunicación.

---

## 6. Lo que NO debes hacer

| No hagas | Por qué |
| --- | --- |
| **Arrancar la bomba 2 o 3 en generador** | Es la causa número uno de apagón del generador |
| **Resetear sin mirar por qué** | Un reset a ciegas convierte un aviso en un fallo recurrente |
| **Forzar una bomba que no responde** | Si no arranca, casi siempre es el contactor. Reintentar lo empeora |
| **Tocar el MCC o el PNOZ con la planta en marcha** | Procedimiento eléctrico, no de operador |
| **Desenclavar el E-Stop sin ir a mirar** | Puede haber alguien en riesgo |
| **Dejar el E-Stop puenteado "para no molestarse"** | Elimina la única protección física |
| **Usar el token de otro** | Todo queda en el audit log con tu nombre |

> **El E-Stop no se puentea. Nunca.** Es lo único que funciona cuando todo
> lo demás falla, incluidas las bombas, el PLC y este manual.

---

## 7. Trip por sobrecarga: qué significa en la práctica

Si ves "SOBRECARGA GENERADOR" seguido de "re-armado automático":

> **No hiciste nada mal y no hay que hacer nada.** El sistema detectó
> demasiado consumo, paró las bombas, esperó y volvió a arrancar. Es
> exactamente lo que debe hacer.

Si ves "intentos agotados":

> El sistema lo intentó 3 veces en 15 minutos y cada vez encontró
> sobrecarga. Eso ya **no** es transitorio. Hay algo real: revisa la
> installation antes de resetear.

| Patrón en el dashboard | Significado |
| --- | --- |
| `0x6001` → `0x6020` (info) | Trip normal, recuperado solo. Normal |
| `0x6001` → `0x6020` → `0x6001` → `0x6020` | Segundo intento. Vigila, pero no acts |
| `0x6001` → `0x6021` (crítico) | Tres intentos. Sí requiere una persona |

---

## 8. Cuando el pozo sube y solo hay una bomba

Si el nivel sube estando en generador, **el sistema no puede más.** Con una
sola bomba no da más de sí, y eso no lo arregla ningún botón.

Esto **sí** es una decisión humana, y solo tuya:

| Opción | Cuándo | Riesgo |
| --- | --- | --- |
| Subir el umbral de nivel del PLC | Nivel alto sostenido | El pozo puede desbordar |
| Conectar la red si hay | Si hay red disponible | Depende del DSE |
| Aceptar y vigilar | Nivel < 95 % | Bajo |
| **Avisar a R2** | **Siempre** | Ninguno |

> El sistema **no** baja las bombas por nivel alto. Lo hace por
> sobrecorriente, que es un hecho eléctrico medido. La decisión de dejar de
> proteger el generador para proteger el pozo es de una persona con
> contexto del lugar. Esa persona eres tú y tu supervisor.

---

## 9. Preguntas frecuentes

**¿Puedo dejar el sistema solo un fin de semana?**
Sí, si el plan de escalamiento declara quién responde. Ver
`docs/escalation_matrix.md`.

**El nivel marca 0 % y sé que hay agua.**
El sensor está desconectado o calibrado mal. Es una avería del sensor, no
una bajada de nivel. No lo ajustes, avisa.

**El LED verde parpadea y no hay nada que hacer.**
Es modo mantenimiento: alguien lo activó a propósito. No lo desactives sin
preguntar.

**Suena la alarma pero las bombas están arrancadas.**
Es un aviso, no una parada. Los avisos no requieren acción inmediata.

**¿Puedo cambiar los umbrales de corriente?**
No. Son el parámetro que protege el generador y su modificación requiere
la prueba de sobrecarga correspondiente (`PT-05`).

**¿Quién puede tocar el relé de seguridad PNOZ?**
Solo R4 (eléctrico), con procedimiento y registro.

---

## 10. A quién llamar

| Rol | Contacto | Cuándo |
| --- | --- | --- |
| R1 — Operador | `PENDIENTE` | — (eres tú) |
| R2 — Supervisor de turno | `PENDIENTE` | Trip agotado, mantenimiento |
| R4 — Eléctrico | `PENDIENTE` | Contactor pegado, fallo de sensor |
| R5 — Seguridad | `PENDIENTE` | Después de un E-Stop |

> **Después de un E-Stop, el retorno a servicio lo autoriza R5, no tú.**
> No es un tecnicismo: si el E-Stop se activó porque alguien se quedó
> atrapado, arrancar sin confirmar el área es cómo se repite el accidente.

---

## 11. Lo que este manual no cubre

- Procedimientos eléctricos (están en `docs/wiring.md`, para R4)
- Actuación de un E-Stop (está en `docs/playbooks.md` PB-06)
- Escalamiento y ventanas (`docs/escalation_matrix.md`)
- Simacros (`docs/drills.md`)

Si algo de tu día a día no está aquí, **falta en este manual.** Dilo.

---

*Fase 6 — Documento 1 de 3. Ver `docs/competencies.md` y
`docs/training_plan.md`.*
