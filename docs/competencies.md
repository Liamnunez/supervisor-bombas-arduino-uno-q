# Competencias por Rol - Fase 6

> Qué tiene que saber y saber hacer cada persona para que su parte del
> sistema de seguridad funcione. Sin esto, el firmware es correcto pero el
> sistema no lo es.

---
> ⚠️ **Placeholders** marcados `PENDIENTE`.

---

## Principio

Las competencias no se derivan del cargo, se derivan de **qué decisiones
tiene que tomar esa persona cuando el sistema no puede decidir por ella**.

Tres cosas hay que separar siempre:

| Nivel | Qué es | Ejemplo |
| --- | --- | --- |
| **Tarea** | Una acción repetible con procedimiento escrito | Resetear emergencia tras 3 trips |
| **Decisión** | Elegir entre opciones con consecuencias | ¿Subo el umbral de nivel o conecto la red? |
| **Criterio** | Saber cuándo NO actuar | Reconocer que "todo va bien" y no tocar nada |

> La tercera es la más difícil de enseñar y la más importante. Un operador
> que interviene sin necesidad es peor que uno que no sabe intervenir, porque
> anula la protección justo cuando hace falta.

---

## R1 — Operador

**Perfil:** personal de turno en planta. **Certificación:** interna.

### Tareas (debe poder hacerlas sin ayuda)

| # | Competencia | Verificación |
| --- | --- | --- |
| 1.1 | Leer los 4 estados en los LEDs | Oral |
| 1.2 | Reconocer un patrón de trip normal (`0x6001`→`0x6020`) | Oral |
| 1.3 | Distinguir "recuperado solo" de "intentos agotados" | Escrita |
| 1.4 | Resetear emergencia **con motivo documentado** | Práctica en planta |
| 1.5 | Activar y desactivar mantenimiento | Práctica en planta |
| 1.6 | Arrancar el sistema tras parada | Práctica en planta |
| 1.7 | Confirmar en el dashboard que hay comunicación | Práctica |

### Decisiones (debe saber decidir con contexto)

| # | Competencia | Verificación |
| --- | --- | --- |
| 1.8 | Ante pozo alto en generador: subir umbral, conectar red o esperar | Caso escrito |
| 1.9 | Ante contactor que no arranca: reintentar o llamar a R4 | Caso escrito |
| 1.10 | Cuándo escalar a R2 en vez de resetear por su cuenta | Caso escrito |

### Criterios (lo más importante)

| # | Competencia | Verificación |
| --- | --- | --- |
| 1.11 | **No arrancar B2/B3 en generador** | Simulacro DR-01 |
| 1.12 | **No resetear sin mirar la causa** | Simulacro DR-01 |
| 1.13 | **No modificar umbrales** | Oral |
| 1.14 | **No tocar MCC ni PNOZ** | Oral |
| 1.15 | Reconocer que algo "va raro" sin ser un fallo | Caso escrito |

### Conocimientos mínimos (R1)

- Por qué el generador no soporta 2 bombas
- Qué significa "fall-safe" y por qué el sistema se para solo
- Que las alertas `warning` **no** requieren acción inmediata
- Cómo abrir el audit log y buscar un comando propio
- Que el token es personal e intransferible

---

## R2 — Supervisor de turno

**Perfil:** encargado de turno. **Certificación:** interna + formación
eléctrica básica.

### Competencias adicionales a R1

| # | Competencia | Verificación |
| --- | --- | --- |
| 2.1 | Autorizar y rechazar una parada de mantenimiento | Caso |
| 2.2 | Aplicar el plan de escalamiento (`escalation_matrix.md`) | Simulacro |
| 2.3 | Distinguir un evento de un incidente | Caso |
| 2.4 | Conducir la declaración de un incidente (fase 1) | Simulacro DR-02 |
| 2.5 | Rellenar el acta de simulacro | Práctica |
| 2.6 | Verificar que las acciones correctivas de un simulacro se cierran | Revisión |
| 2.7 | Saber cuándo escalar a R4 sin demora | Caso |

### Conocimientos añadidos (R2)

- La matriz de severidades y qué respuesta exige cada nivel
- La diferencia entre evento, incidente y accidente
- Los cinco simulacros DR-01..DR-05 y su alcance
- Que un simulacro se firma **aunque falle**

---

## R3 — Administrador del sistema

**Perfil:** responsable de telemetría / sistemas. **Certificación:** interna.

### Competencias adicionales a R2

| # | Competencia | Verificación |
| --- | --- | --- |
| 3.1 | Gestión de tokens y roles (RBAC) | Práctica |
| 3.2 | Auditar el `audit.log` JSONL | Práctica |
| 3.3 | Verificar que el sistema es **fail-closed** | Práctica |
| 3.4 | Conocer el riesgo de `set_modo_generador` remoto | Oral firmado |
| 3.5 | Actualizar firmware siguiendo el procedimiento de fase 4 | Práctica |
| 3.6 | Reconocer una firma de imagen inválida | Caso |
| 3.7 | Mantener el plan de escalamiento y los contactos | Revisión |

### Conocimientos añadidos (R3)

- Por qué la clave privada de firma no está en el repo ni en la planta
- Por qué una actualización de firmware **invalida** PT-01..PT-09
- Por qué la activación remota de firmware no se recomienda
- La diferencia entre "firma válida" y "firmware probado"

> **3.4 requiere firma escrita.** Es la excepción documentada al principio del
> proyecto: `set_modo_generador` remoto desactiva el interlock de 1 bomba.
> Quien lo usa debe saberlo por escrito.

---

## R4 — Responsable eléctrico

**Perfil:** electricista / técnico de mantenimiento. **Certificación:**
habilitación eléctrica + formación del sistema.

### Competencias

| # | Competencia | Verificación |
| --- | --- | --- |
| 4.1 | Ejecutar PT-01..PT-09 completos | Firma de cada PT |
| 4.2 | Diagnosticar un contactor pegado | Caso real |
| 4.3 | Verificar el retorno auxiliar NO tras cambio de contactor | Práctica |
| 4.4 | Montar y cablear el PNOZ s4 en modo watchdog | Práctica + PT-09 |
| 4.5 | Ejecutar PT-10..PT-15 cuando exista bootloader | Firma de cada PT |
| 4.6 | Calibrar el CT de corriente | Práctica con pinza |
| 4.7 | Usar PNOZ en seguro (energizar/desenergizar) | Oral firmado |
| 4.8 | Registrar una intervención con evidencia | Práctica documental |

### Conocimientos mínimos (R4)

- Por qué los relés son NO con muelle (fail-safe pasivo)
- Qué pasa si se corta el cable del modo (→ GENERADOR, restrictivo)
- Por qué la malla de los cables de señal se pone **en un solo extremo**
- Por qué el latido del PNOZ va en reposo LOW

> **4.7 es crítico.** Trabajar sobre un centro de mando sin el PNOZ en
> seguro puede arrancar una bomba que el sistema creía parada.

---

## R5 — Responsable de seguridad

**Perfil:** responsable de seguridad / EHS. **Certificación:** formación en
funciones de seguridad (IEC 61508 nivel introductorio).

### Competencias específicas (R5)

| # | Competencia | Verificación |
| --- | --- | --- |
| 5.1 | Autorizar retorno a servicio tras E-Stop | Simulacro DR-02 |
| 5.2 | Entender la matriz SIF y su relación con HAZOP | Oral |
| 5.3 | Entender el riesgo residual del LOPA | Oral |
| 5.4 | Rechazar un simulacro mal ejecutado | Caso |
| 5.5 | Firmar el acta de incidente grave | Práctica |
| 5.6 | Exigir PT-01..PT-09 tras **cualquier** cambio de firmware | Oral firmado |

> **5.6 es la que protege la planta.** Un firmware nuevo sin re-ejecutar los
> PT deja la instalación sin evidencia de que su función de seguridad sigue
> siendo la misma. La firma del artefacto no sustituye a las pruebas.

---

## Separación de funciones

| Se exige separación entre… | Porque… |
| --- | --- |
| Quien **repara** y quien **autoriza** el retorno | El que arregla tiende a ver su arreglo como bueno |
| Quien **opera** y quien **verifica** la firma del firmware | Un admin no debe poder instalar firmware sin validación |
| Quien **opera** y quien **libera** el mantenimiento | Evita que una bomba se deje parada "sin querer" |

> Si la planta tiene una sola persona, esa persona acumula los roles. **Eso
> está permitido y debe estar escrito** como limitación conocida en el
> informe de riesgo, no omitido en silencio. Ver `docs/hazop_lopa.md`.

---

## Requisitos comunes a todos

| # | Competencia | Verificación |
| --- | --- | --- |
| C.1 | Saber dónde está el E-Stop y cómo funciona | Práctica |
| C.2 | Saber que el E-Stop no se puentea **bajo ninguna circunstancia** | Oral firmado |
| C.3 | Saber a quién llamar y en qué plazo | Oral |
| C.4 | Leer los 4 estados en el dashboard | Práctica |
| C.5 | Interpretar los códigos `0x6001` / `0x6010` / `0x6020` / `0x6021` | Escrita |
| C.6 | Saber que el audit log registra **todas** sus acciones | Oral |
| C.7 | Tener su token personal y no compartirlo | Oral firmado |

---

## Matriz de cobertura

| Competencia | R1 | R2 | R3 | R4 | R5 |
| --- | --- | --- | --- | --- | --- |
| Lectura de estados y alertas | 1.1–1.3, 1.7 | hereda | hereda | hereda | hereda |
| Reset con motivo | 1.4 | hereda | hereda | hereda | hereda |
| Mantenimiento | 1.5 | 2.1 | hereda | hereda | hereda |
| Decisión de pozo alto | 1.8 | 2.2 | hereda | — | — |
| No intervenir sin necesidad | 1.11–1.12 | hereda | hereda | hereda | hereda |
| Escalamiento | 1.10 | 2.2 | hereda | hereda | hereda |
| RBAC y auditoría | — | — | 3.1–3.3 | — | — |
| Riesgo del override remoto | — | — | 3.4 (firmado) | — | 5.3 |
| Firmware y proof tests | — | — | 3.5–3.7 | 4.5 | 5.6 (firmado) |
| Eléctrico / PNOZ | — | — | — | 4.1–4.8 | — |
| Retorno tras E-Stop | — | 2.4 | — | 4.8 | 5.1 (autoriza) |

---

*Fase 6 — Documento 2 de 3. Ver `docs/operator_manual.md` y
`docs/training_plan.md`.*
