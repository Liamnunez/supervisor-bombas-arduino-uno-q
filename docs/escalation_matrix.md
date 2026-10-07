# Matriz de Escalamiento - Fase 5

> Quién atiende cada nivel de severidad, por qué canal y en cuánto tiempo.

---

> ⚠️ **DOCUMENTO NO APROBADO.** Todos los campos marcados `PENDIENTE` deben
> completarse con personal real antes de usar este documento en operación.
> Sin datos de planta, este archivo es una plantilla, no un procedimiento.

---

## Roles

| Rol | Persona | Contacto | Puede |
| --- | --- | --- | --- |
| **R1 — Operador** | `PENDIENTE` | `PENDIENTE` | Ver estado, pedir arranque/parada |
| **R2 — Supervisor de turno** | `PENDIENTE` | `PENDIENTE` | Resetear emergencia, abrir/cerrar mantenimiento |
| **R3 — Administrador** | `PENDIENTE` | `PENDIENTE` | Todo lo anterior + cambiar modo generador |
| **R4 — Responsable eléctrico** | `PENDIENTE` | `PENDIENTE` | intervene el MCC, repara contactores |
| **R5 — Responsable de seguridad** | `PENDIENTE` | `PENDIENTE` | Autoriza retorno a servicio tras C-06/C-07 |

> La separación de roles **no** depende de que haya personas distintas: si la
> planta tiene una sola, esa persona acumula todos los roles y queda
> registrado como limitación Known en el informe de riesgo. Ver
> `docs/security_protocols.md` §4.

---

## Matriz: severidad → quién → cuándo

| Severidad | Responde | Ventana | Canal primario | Canal alterno |
| --- | --- | --- | --- | --- |
| **INFO** | Nadie | — | Dashboard | — |
| **WARNING** | Nadie (observa) | Si se repite → R2 en 24 h | Telegram | Email |
| **CRITICAL** | Ver abajo | 15 min | Telegram + llamada | SMS + llamada |
| **E-Stop (C-06)** | R4 + R5 | Inmediato | Llamada telefónica | Radio / visita |
| **MCU caído (C-07)** | R2 o R3 | 30 min | Telegram + llamada | Visita |

---

## Ventanas de respuesta

| Objetivo | Plazo | Justificación |
| --- | --- | --- |
| Acuse de recibo de WARNING | Ninguno | El sistema ya se auto-recupera |
| Acuse de recibo de CRITICAL | 15 min | Confirmar que alguien lo vio |
| Presencial en planta tras C-06 | 30 min | E-Stop manual = hay una persona cerca |
| Presencial en planta tras C-07 | 60 min | Requiere acceso al MCC |
| Retorno a servicio | Por R5 | No se reactiva solo tras E-Stop |

> La ventana de 15 min **no bloquea la seguridad**: el sistema ya está
> protegido por hardware. La ventana es para que alguien *se entere*, no
> para que el sistema espere.

---

## Criterios de escalamiento

| Si... | Escala a |
| --- | --- |
| CRITICAL sin acuse en 15 min | R2 → R3 → R4 |
| Dos CRITICAL del mismo tipo en 1 h | R3 directamente |
| Tres CRITICAL en 24 h | R4 + R5, revisión de umbrales |
| C-06 (E-Stop) en cualquier momento | R4 + R5, sin esperar ventana |
| C-07 se repite 3 veces | R4 (posible fallo de alimentación del MCU) |
| Nivel crítico alto > 90 % durante 4 h | R2 (probable fallo de bomba, no de protección) |

---

## Cobertura horaria

| Franja | Quién cubre | Canal |
| --- | --- | --- |
| Lunes a viernes 08:00–18:00 | R1/R2 en planta | Telegram + walkedie |
| Lunes a viernes 18:00–08:00 | `PENDIENTE` | `PENDIENTE` |
| Sábados, domingos, festivos | `PENDIENTE` | `PENDIENTE` |

> ⚠️ **Este es el hueco más probable del plan.** Si no hay nadie en la
> franja nocturna, la respuesta real a un CRITICAL es "el sistema está
> protegido y esperando al turno de mañana". Eso **es aceptable** para un
> sistema fail-safe, pero debe estar escrito y aceptado, no implícito.

---

## Alternativa si no hay cobertura nocturna

Si no hay personal nocturno, el plan de escalamiento se reduce a:

1. **INFO** → nadie, nunca.
2. **WARNING** → nadie, nunca.
3. **CRITICAL de sistema (C-01..C-05, C-08)** → el firmware ya paró lo
   necesario. Alerta a Telegram para revisión en el siguiente turno.
4. **CRITICAL humano (C-06, C-07)** → TELEFONO a R4/R5. Cualquier hora.

En ese modelo, la **única** llamada a deshora es por E-Stop o MCU caído,
porque son los dos casos en que el equipo queda parado hasta que llegue
alguien.

---

## Contactos

| Rol | Nombre | Teléfono | Email | Telegram |
| --- | --- | --- | --- | --- |
| R1 | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` |
| R2 | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` |
| R3 | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` |
| R4 | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` |
| R5 | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` | `PENDIENTE` |

| Servicio | Teléfono |
| --- | --- |
| Emergency (bomberos) | `PENDIENTE` |
| Servicio eléctrico | `PENDIENTE` |
| Proveedor de contactores | `PENDIENTE` |
| Proveedor del PNOZ s4 | `PENDIENTE` |

---

## Verificación periódica

| Qué | Cada | Quién |
| --- | --- | --- |
| Números de contacto siguen vigentes | 6 meses | R3 |
| Alertas Telegram llegan al chat correcto | 3 meses | R2 (simulacro) |
| Turno nocturno declarado y vigente | 6 meses | R3 |

---

Fase 5 — Documento 1 de 5
