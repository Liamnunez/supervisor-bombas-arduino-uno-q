# Procedimiento de Respuesta a Incidentes - Fase 5

> Cómo se declara, comunica, recupera y revisa un incidente de seguridad.

---

> ⚠️ **DOCUMENTO NO APROBADO.** Requiere datos de personal
> (`docs/escalation_matrix.md`) y un simulacro ejecutado antes de tener valor
> operativo.

---

## Principio rector

> **Un incidente de seguridad no se maneja apagando la protección.**

El sistema para bombas por diseño, no por avería. Ante un evento, la
prioridad es entender qué pasó, no recuperar la producción a costa de
ocultar el fallo.

---

## Definición de incidente

| Tipo | Ejemplo | Procedimiento |
| --- | --- | --- |
| **Evento** | Trip que se auto-recuperó | Registrar, no investigar en profundidad |
| **Incidente** | 3 trips en 15 min | Playbook + revisión |
| **Accidente** | E-Stop por riesgo a persona | playbooks PB-06 + investigación completa |

> La distinción importa: si cada evento auto-recuperado abriera una
> investigación formal, nadie investigaría nada porque el sistema
> generaría decenas de alertas al día.

---

## Fase 1 — Detección y declaración

### ¿Quién declara?

El **sistema** declara automáticamente. La persona no decide si hay
incidente; confirma o escala.

```text
CRITICAL en dashboard / Telegram
        │
        ├── Sin acuse en 15 min → escala automático (R2 → R3 → R4)
        │
        └── Acuse → confirmar que la alerta es real
                     │
                     ├── Falsa alarma → cerrar con nota (ver §Verdadero/falso)
                     └── Real → Fase 2
```

### Efectos de la declaración

| Efecto | Detalle |
| --- | --- |
| Alertas congeladas | Se silencia el canal Telegram hasta el acuse |
| Log sellado | Se hace snapshot del audit log + metrics + logs del MCU |
| Ventana de evidencia | 7 días antes de que la rotación borre los datos |

> El "sellado" es lo que hace posible investigar tres meses después. Sin
> él, cualquier incidente con retención de 30 días es ininvestigable.

---

## Fase 2 — Contención

**Objetivo:** evitar que la avería se convierta en un accidente mientras se
investiga.

| Acción | Quién | Cuándo |
| --- | --- | --- |
| Verificar que las bombas están en estado seguro | Automático | Inmediato |
| No manipular el MCC | Todos | Hasta Fase 3 |
| Preservar evidencia (fotos, lectura de CT, estado del PNOZ) | R4 | Antes de tocar nada |
| Si hay riesgo a persona, activar E-Stop | R1/R2 | Inmediato |

> **No reiniciar para "ver si era algo".** Un reinicio Borra evidencia y
> puede devolver el sistema al estado que causó el incidente.

---

## Fase 3 — Investigación

### Fuentes de evidencia

| Fuente | Qué aporta | Retención |
| --- | --- | --- |
| `SUPERVISOR_AUDIT_LOG` (JSONL) | Quién pidió qué y cuándo | 1 año |
| SQLite `alerts` | Línea de tiempo de eventos | 90 días |
| SQLite `metrics` | Corrientes, niveles, Duty cycle | 30 días |
| Serial del MCU (`115200`) | Decisiones internas del firmware | Solo si se guardó |
| Estado del PNOZ s4 | Si hubo apertura por hardware | Actual |
| Medición directa con pinza | Verdad de campo | Manual |

> El **serial del MCU es la fuente más valiosa y la que más se pierde.**
> Recomendación operativa: `make mcu-monitor \| tee -a /var/log/mcu.log`
> con rotación. Implementado en Fase 6.

### Preguntas guía

1. ¿El evento fue causado por el sistema o por una condición externa?
2. ¿La protection actuated antes o después del DSE 7320?
3. ¿Hubo señales contradictorias (orden sin feedback, feedback sin orden)?
4. ¿Los umbrales eran los correctos para la carga real?
5. ¿Alguien tenía una sesión abierta cuando pasó?

### Salida

Un **informe de causa** con:

- Línea de tiempo reconstructiva
- Causa raíz (o "no determinada", que es una respuesta válida)
- Acción correctiva
- Verificación de que la acción correctiva funciona

---

## Fase 4 — Recuperación

### Orden de retorno a servicio

| # | Paso | Quién | Condición |
| --- | --- | --- | --- |
| 1 | Causa identificada | R4 | Informe firmado |
| 2 | Acción correctiva aplicada | R4 | Verificado |
| 3 | Proof test ejecutado (PT-xx) | R4 | Pass |
| 4 | Autorización de retorno | R5 | Firmada |
| 5 | Reset de emergencia | R2/R3 | Token `operator` |
| 6 | Verificación de arranque | R2 | Bombas respondiendo |
| 7 | Observación 24 h | R2 | Sin recurrencia |

> **El paso 4 no es opcional y no lo hace el técnico.** Es la separación
> entre quien arregla y quien autoriza. En una planta con una sola persona,
> esa separación se documenta como limitación, no se omite en silencio.

### Después del retorno

- [ ] Alertas de Telegram reactivadas
- [ ] Incidente cerrado en el registro
- [ ] Repetido en el simulacro trimestral (`docs/drills.md`)
- [ ] Si fue un evento repetido → revisar umbrales

---

## Fase 5 — Revisión (post-incidente)

### Cuándo es obligatoria

| Tipo | Revisión |
| --- | --- |
| Evento auto-recuperado | Mensual, agregada |
| Incidente | En 15 días |
| Accidente (E-Stop por persona) | Antes de reabrir, con R5 |

### Qué produce

| Documento | Contenido |
| --- | --- |
| Acta de incidente | Cronología, causa, impacto |
| Acciones correctivas | Con responsable y fecha |
| Cambios en procedimiento | Qué se actualiza en PB-xx |
| Cambios en firmware | Backlog, con proof test asociado |

### Regla de oro

> **Cada incidente debe cambiar algo.** Si el procedimiento era correcto y
> el hardware estaba bien, el incidente también cambia algo: el umbral, la
> sensibilidad de la alerta, o el tiempo de respuesta.

Un incidente sin acción correctiva es un accidente que va a repetirse.

---

## Falsos positivos

Un sistema con cero falsos positivos no existe; uno con demasiados deja de
alertar. Criterio de gestión:

| Frecuencia | Acción |
| --- | --- |
| 1 falso positivo al mes | Aceptable |
| 3 al mes | Revisar umbral del sensor |
| 5 al mes | Revisar umbral del software |
| 10 al mes | **El canal de alerta no es válido** |

Un canal de alerta ignorado es peor que no tener alerta: entrena al equipo a
ignorar.

---

## Registro de incidentes

| # | Fecha | Severidad | Escenario | Duración | Causa | Cerrado | Informe |
| --- | --- | --- | --- | --- | --- | --- | --- |
| — | — | — | — | — | — | — | — |

> *Vacío de forma intencionada: no se documentan incidentes inventados. La
> primera fila se rellena con el primer evento real.*

---

Fase 5 — Documento 4 de 5
