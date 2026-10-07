# Plan de Formación - Fase 6

> Cómo se entrena, cómo se verifica que el entrenamiento funcionó, y qué
> pasa si no se hace.

---
> ⚠️ **Placeholders:** nombres, fechas y responsables marcados `PENDIENTE`.
> Ningún simulacro es válido hasta que la Fase 3 y el commissioning estén
> completos.

---

## Principio: se evalúa la conducta, no la asistencia

La formación no se valida por "asistió al curso". Se valida por lo que la
persona **hace** cuando el sistema dice algo incómodo.

El indicador más revelador es del simulacro DR-01: **¿intervino el operador
cuando no debía?** Un operador que resetea por reflejo ha "./formado" aunque
tenga todos los diplomas.

---

## Estructura

| Fase | Duración | Participantes | Objetivo |
| --- | --- | --- | --- |
| **F0 — Concienciación** | 2 h | Todos | Saber que el sistema es automático |
| **F1 — Operador (R1)** | 4 h + práctica | R1 | Operar y, sobre todo, no tocar |
| **F2 — Supervisor (R2)** | 4 h | R2 | Escalamiento e incidentes |
| **F3 — Administración (R3)** | 4 h | R3 | RBAC, auditoría, firmware |
| **F4 — Eléctrico (R4)** | 8 h + práctica | R4 | PT, PNOZ, sensores |
| **F5 — Seguridad (R5)** | 4 h | R5 | SIF, LOPA, retorno tras E-Stop |
| **Reciclaje** | 2 h/año | Todos | Después de cada cambio de sistema |

---

## F0 — Concienciación (2 h, todos)

**Objetivo único:** que nadie intente arreglar el sistema porque algo "parece"
ir mal.

| # | Contenido | Min |
| --- | --- | --- |
| 0.1 | Qué es el supervisor y por qué existe entre el PLC y las bombas | 20 |
| 0.2 | El generador no soporta 2 bombas. Esa es la razón de todo | 15 |
| 0.3 | El sistema es automático: no se toca en operación normal | 20 |
| 0.4 | Los 4 estados y sus LEDs | 20 |
| 0.5 | Qué es un E-Stop y por qué no se puentea jamás | 25 |
| 0.6 | A quién llamar y en qué plazo | 20 |

> **La sección 0.5 es la que más se repite y la que más se olvida.** Sin
> ella, un simulacro de DR-01 acaba con alguien preguntando si se puede
> dejar el E-Stop puenteado mientras se arregla algo.

---

## F1 — Operador (4 h + práctica en planta)

**Material:** `docs/operator_manual.md` completo.

| # | Contenido | Min |
| --- | --- | --- |
| 1.1 | Los 4 estados, los LEDs, el dashboard | 30 |
| 1.2 | Códigos `0x6001` / `0x6010` / `0x6020` / `0x6021` y qué hacer con cada uno | 30 |
| 1.3 | Reset de emergencia **con motivo documentado** | 30 |
| 1.4 | Mantenimiento: cuándo y cómo | 20 |
| 1.5 | Arranque tras parada larga | 20 |
| 1.6 | **Decisiones con contexto**: pozo alto en generador | 40 |
| 1.7 | **Qué NO hacer** (revisión de la lista completa) | 30 |

### Práctica supervised (obligatoria)

| Prueba | Criterio de aprobado |
| --- | --- |
| Resetear con motivo válido | Motivo escrito, causa revisada antes |
| Activar/desactivar mantenimiento | Avisa a R2 antes |
| Arrancar tras parada | Verifica comunicación, no arranca bombas a mano |

---

## F2 — Supervisor (4 h)

| # | Contenido | Min |
| --- | --- | --- |
| 2.1 | Matriz de severidades y respuesta requerida | 40 |
| 2.2 | `docs/escalation_matrix.md` y ventanas de respuesta | 30 |
| 2.3 | Evento vs incidente vs accidente | 30 |
| 2.4 | Declarar un incidente (fase 1 de `incident_response.md`) | 40 |
| 2.5 | Rellenar el acta de simulacro | 30 |
| 2.6 | Seguimiento de acciones correctivas | 30 |

---

## F3 — Administración (4 h)

| # | Contenido | Min |
| --- | --- | --- |
| 3.1 | RBAC: qué puede cada rol y por qué `set_modo_generador` es de admin | 40 |
| 3.2 | Gestión de tokens; el token es personal | 20 |
| 3.3 | Audit log JSONL: buscar una acción propia | 30 |
| 3.4 | Verificar que el sistema es fail-closed | 30 |
| 3.5 | **Riesgo del override remoto** de modo generador | 30 |
| 3.6 | Procedimiento de actualización de firmware (`ota_procedure.md`) | 40 |
| 3.7 | Por qué un firmware nuevo invalida PT-01..PT-09 | 20 |

> **3.5 requiere firma escrita de quien asista.** Es la excepción
> documentada al principio del proyecto. Ver `docs/competencies.md` R3-3.4.

---

## F4 — Eléctrico (8 h + práctica)

| # | Contenido | Min |
| --- | --- | --- |
| 4.1 | Bornas, cableado, separación de potencia y señal | 60 |
| 4.2 | Relés NO con muelle: por qué el sistema se para sin corriente | 30 |
| 4.3 | Contactores y retornos auxiliares NO | 45 |
| 4.4 | **Pilz PNOZ s4**: cableado, modo watchdog, uso en seguro | 90 |
| 4.5 | CT de corriente y su calibración | 45 |
| 4.6 | Sensor de nivel 4-20 mA y resistencia shunt | 30 |
| 4.7 | Ejecutar PT-01..PT-09 completos | 120 |
| 4.8 | Registro de intervención con evidencia | 30 |

> **4.7 es la parte que hace válida la instalación.** Sin PT firmados, el
> sistema no tiene evidencia de que su función de seguridad funciona.

---

## F5 — Seguridad (4 h)

| # | Contenido | Min |
| --- | --- | --- |
| 5.1 | Qué es una SIF y cuál es su SIL objetivo | 45 |
| 5.2 | HAZOP y LOPA: riesgo residual aceptado | 60 |
| 5.3 | Por qué el PNOZ en watchdog importa (SIF-06) | 30 |
| 5.4 | **Autorizar retorno a servicio tras un E-Stop** | 45 |
| 5.5 | Rechazar un simulacro mal ejecutado | 30 |

---

## Reciclaje

| Cuándo | Qué | Duración |
| --- | --- | --- |
| Anual, todos | Simacro completo DR-05 | included |
| Semestral | Cambios del sistema desde el último reciclaje | 1 h |
| Tras cualquier cambio de firmware | Por qué se re-ejecutan los PT | 30 min |
| Al incorporarse alguien nuevo | F0 + F1 completas | 6 h |

> **Reciclaje tras cambio de firmware es obligatorio.** No es burocracia: el
> cambio puede haber alterado algo que la persona memorizó.

---

## Evaluación

| Nivel | Método | Aprobado |
| --- | --- | --- |
| Conocimiento | Preguntas escritas (mínimo 80 %) | ≥ 80 % |
| Habilidad | Práctica supervised en planta | Todos los criterios |
| **Criterio** | Simacro DR-01: **no intervenir sin necesidad** | 0 intervenciones indebidas |
| Criterio crítico | Todos los roles: E-Stop nunca puenteado | Firma |

> El criterio de "criterio" es eliminatorio. Quien interviene de más en DR-01
> repite F1, aunque tenga el resto de las notas.

---

## Registro

| Persona | Rol | F0 | F1 | F2 | F3 | F4 | F5 | Reciclaje | Apto |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| `PENDIENTE` | R1 | | | | | | | | |
| `PENDIENTE` | R2 | | | | | | | | |
| `PENDIENTE` | R3 | | | | | | | | |
| `PENDIENTE` | R4 | | | | | | | | |
| `PENDIENTE` | R5 | | | | | | | | |

*Vacío de forma intencionada: no se documenta formación inexistente.*

---

## Qué pasa si no se hace

| Si no se forma a… | Consecuencia |
| --- | --- |
| R1 | Reset a ciegas, o intervención innecesaria que anula la protección |
| R2 | Escalamiento que no llega a tiempo; incidentes sin investigar |
| R3 | Firmware instalado sin re-ejecutar PT; override remoto sin conocer el riesgo |
| R4 | PT sin ejecutar; instalación sin evidencia de seguridad |
| R5 | Retorno a servicio sin evaluar: el accidente se repite |

> **La fila de R1 es la más grave.** Un operador resetando por reflejo no
> rompe nada visible, y por eso el daño pasa desapercibido hasta que importa.

---

## Antes de formar

| Requisito | Estado |
| --- | --- |
| Hardware instalado y commissioning completo | `PENDIENTE` |
| PT-01..PT-09 ejecutados al menos una vez | `PENDIENTE` |
| Panel con E-Stop y PNOZ operativo | `PENDIENTE` |
| `escalation_matrix.md` con contactos reales | `PENDIENTE` |
| `operator_manual.md` con placeholders completados | `PENDIENTE` |
| Telemetría funcionando (llegada de Telegram verificada) | `PENDIENTE` |

> **No se forma sobre un sistema que no está montado.** Los cinco
> requisitos previos están marcados `PENDIENTE` porque dependen de hardware y
> de datos de planta que aún no existen.

---

*Fase 6 — Documento 3 de 3. Ver `docs/operator_manual.md` y
`docs/competencies.md`.*
