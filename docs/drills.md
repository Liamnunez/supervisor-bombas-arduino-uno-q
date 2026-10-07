# Simulacros y Verificación Periódica - Fase 5

> Un plan de respuesta a incidentes que nunca se ha probado no es un plan,
> es un documento.

---

> ⚠️ **DOCUMENTO NO APROBADO.** Ningún simulacro es válido hasta que la
> Fase 3 (hardware) y el commissioning estén completos.

---

## Distinción importante: proof test ≠ simulacro

| | Proof test (PT-xx) | Simulacro |
| --- | --- | --- |
| **Qué prueba** | El equipo técnico | El equipo humano |
| **Frecuencia** | Semestral/anual (`hardware_spec.md` §8) | Trimestral |
| **Requiere parar la planta** | Sí | No |
| **Falla esperada si no se hace** | El SIF no es creíble | El equipo no sabe reaccionar |
| **Quién ejecuta** | R4 | R2 + 1 participante no esperado |

> Un PT que falla es un **defecto de seguridad**. Un simulacro que falla es
> un **defecto de preparación**. Ambos necesitan acción, pero solo uno
> detiene la planta.

---

## Calendario

| Trimestre | Simulacro | Tipo | Participantes |
| --- | --- | --- | --- |
| **T1** | DR-01 Tormenta en generador | Tabla | R2, R3 |
| **T2** | DR-02 E-Stop + recuperación | Física | R2, R4, R5 |
| **T3** | DR-03 Trip por sobrecorriente | Física | R2, R4 |
| **T4** | DR-04 MCU caído | Física | R2, R3 |
| **Anual** | DR-05 Simulacro completo | Física | Todos + dirección |

> **Los simulacros de tabla (DR-01) no requieren parar nada.** Se hacen en
> el dashboard o con un PC en simulación. Son los más frecuentes porque
> son los únicos que se pueden hacer sin pedir ventana de parada.

---

## DR-01 — Tormenta en generador (simulacro de mesa)

**Objetivo:** comprobar que el equipo **no interviene** cuando el sistema
ya lo resuelve solo.

**Preparación:** simular estado GENERADOR con el nivel del pozo subiendo por
encima del 90 %. Sin tocar la planta real.

**Guion:**

| Min | Observador dice | Participante debe responder |
| --- | --- | --- |
| 0 | "Estamos en generador, el pozo sube al 92 %" | Nada. Es automático. |
| 5 | "El PLC ordena la bomba 2" | Nada. El sistema la bloquea. |
| 10 | "¿Qué haces?" | "Nada. Está funcionando como debe." |
| 12 | "El pozo llega al 95 %" | "Esto **sí** requiere decisión: subir umbral, conectar red, o esperar." |

**El punto del simulacro:** el éxito es que el participante **no toque
nada** en los primeros 10 minutos. El fallo más común es que alguien Corra
a pulsar el reset de la emergencia sin que haya ninguna emergencia.

**Verificación técnica:** confirmar en el audit log que el sistema bloqueó
la bomba 2 con el evento correcto.

---

## DR-02 — E-Stop y recuperación (simulacro físico)

**Objetivo:** comprobar que el equipo sabe que un E-Stop **no se reinicia
solo** y que requiere autorización.

**⚠️ Requiere ventana de parada de bombas. Pre-aviso a R5.**

**Guion:**

| Paso | Acción | Verificación |
| --- | --- | --- |
| 1 | Registra hora de inicio | Acta |
| 2 | Presiona E-Stop | Las 3 bombas paran en <50 ms |
| 3 | Espera 5 min **sin hacer nada** | Nadie re-arma por iniciativa propia |
| 4 | Activa el canal de alerta | Llega a R4/R5 |
| 5 | Invententa causa (atrapamiento simulado) | Acta |
| 6 | Desenclava + reset PNOZ (`Y1-Y2`) | Relés listos |
| 7 | **R5 autoriza** el retorno | Firma |
| 8 | Arranque gradual | B1 responde |
| 9 | Registra hora de fin + tiempo total | Acta |

**Métrica:** tiempo desde el paso 2 al paso 7.

| Resultado | Evaluación |
| --- | --- |
| < 30 min | Correcto |
| 30–60 min | Aceptable, revisar vía de aviso |
| > 60 min | **Plan de escalamiento insuficiente** |
| Alguien re-armó sin autorización | **Fallo grave de formación** |

---

## DR-03 — Trip por sobrecorriente (simulacro físico)

**Objetivo:** verificar la auto-recuperación de 3 intentos y que el
participante entiende que los intentos 1 y 2 **no requieren acción**.

**Guion:**

| Paso | Acción | Esperado |
| --- | --- | --- |
| 1 | Inyectar carga > 42 A (pinza + carga resistiva) | Corriente sube |
| 2 | Esperar trip | Relés abren, Telegram avisa |
| 3 | No hacer nada, esperar reintento | Re-arma solo |
| 4 | Observar intento 2 | WARNING |
| 5 | Observar intento 3 | CRITICAL, latched |
| 6 | Resetear desde panel con token | Requiere R2/R3 |
| 7 | Reducir carga, arrancar | Normal |

**Métrica:** ¿el participante entendió que 1 y 2 son automáticos?

> Un participante que resetea en el paso 3 está **saltándose la validación
> del auto-recuperación**. El simulacro pasa, pero se ha probado menos de
> lo previsto.

---

## DR-04 — MCU caído (simulacro físico)

**Objetivo:** verificar que el PNOZ s4 abre los relés cuando el firmware
deja de latir.

**Guion:**

| Paso | Acción | Esperado |
| --- | --- | --- |
| 1 | Estado normal, 2 bombas en marcha | Registro |
| 2 | Cortar alimentación 24 V del MCU | Relés abren en <100 ms |
| 3 | Confirmar PNOZ en estado seguro | LED de fallo |
| 4 | Restaurar 24 V | MCU arranca, PNOZ se rearma |
| 5 | Verificar arranque | según lo ordene el PLC |
| 6 | Cortar **solo** el cable del latido PB3 | Relés abren (¡y el MCU está vivo!) |

> El paso 6 es el que de verdad importa: demuestra que la barrera de
> seguridad depende del cable y no del software. Si el paso 6 no abre los
> relés, el PNOZ **no está en modo watchdog** y todo el diseño de
> seguridad se apoya solo en el firmware.

---

## DR-05 — Simulacro anual completo

Incluye los 4 anteriores **en secuencia, sin avisar**, más:

| Extra | Verificación |
| --- | --- |
| Seguridad física | El cable de latido no es manipuleable por personal no autorizado |
| Invulnerabilidad | Intento de arranque remoto de la 2.ª bomba en GENERADOR |
| Fail-closed | Password incorrecta → comando **rechazado**, no ignorado |
| Evidencia | El audit log tiene todo lo ocurrido |
| Comunicación | Llegó a los canales declarados en `escalation_matrix.md` |
| Continuidad | Funciona con la red caída y con el generador activo |

---

## Plantilla de acta

```text
SIMULACRO DR-XX — FECHA: ________  HORA INICIO: ______  FIN: ______

PARTICIPANTES
  R1: ____________  R2: ____________  R3: ____________
  R4: ____________  R5: ____________  Observador: ________

ESCENARIO: ______________________________
TIPO:      [ ] Mesa   [ ] Físico   [ ] Completo

RESULTADOS
  Objetivo del simulacro cumplido:      [ ] Sí   [ ] Parcial  [ ] No
  Detección de la alerta:               Tiempo: ______ min
  Notificación a R4/R5:                 Tiempo: ______ min
  Recuperación:                         Tiempo: ______ min
  Evidencia registrada correctamente:   [ ] Sí   [ ] No
  La protección actuó en la dirección segura: [ ] Sí  [ ] No

DESVIACIONES
  1. ______________________________________________________________
  2. ______________________________________________________________
  3. ______________________________________________________________

ACCIONES CORRECTIVAS (cada incidente debe cambiar algo)
  #1  Descripción: __________________  Responsable: ____  Fecha: ____
  #2  Descripción: __________________  Responsable: ____  Fecha: ____
  #3  Descripción: __________________  Responsable: ____  Fecha: ____

CAMBIOS EN PROCEDIMIENTO O FIRMWARE
  ______________________________________________________________

EVALUACIÓN GLOBAL:  [ ] Aprobado   [ ] Aprobado con acciones   [ ] Suspenso

Firmas:  R2 ____________   R3 ____________   R4 ____________
         R5 ____________   Observador ____________
```

> **Regla:** el simulacro se firma **aunque falle**. Un simulacro sin acta
> es un simulacro que no ocurrió, y sin acta no hay forma de demostrar que
> la preparación funcionó.

---

## Métricas de preparación

| Indicador | Objetivo |
| --- | --- |
| Simulacros ejecutados / previstos | ≥ 90 % |
| Acciones correctivas cerradas en plazo | 100 % |
| Detección de alerta < 1 min | 100 % |
| Intervención no autorizada en D-R01/03 | 0 |

> El último es el indicador más importante: mide si el equipo **confía en
> la automatización**. Si el equipo toca el sistema cuando no debe, el
> diseño —aunque sea correcto— no está funcionando.

---

Fase 5 — Documento 5 de 5
