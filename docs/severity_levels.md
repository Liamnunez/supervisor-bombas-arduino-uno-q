# Niveles de Severidad y Respuesta Requerida - Fase 5

> Define qué tan grave es un evento y **qué tiene que hacer el sistema y qué
> tiene que hacer una persona** en cada caso.

---

> ⚠️ **Placeholders:** toda persona, teléfono, horario y canal marcado como
> `PENDIENTE` debe completarse antes del simulacro. Ver
> `docs/escalation_matrix.md`.

---

## Principio de diseño: el sistema es autónomo

El supervisor **no requiere operador en operación normal**. Solo hay tres
motivos por los que una persona interviene:

| Motivo | Ejemplo |
| --- | --- |
| **Arrancar** | Recoverer después de una parada larga |
| **Detener** | Parada programada / mantenimiento |
| **Resetear** | Re-armar después de 3 intentos fallidos |

Todo lo demás —tormentas, picos de nivel, conmutación a generador, arranque
de bombas— lo resuelve el firmware sin intervención. Ver `docs/playbooks.md`.

---

## Niveles de severidad

```text
                    ┌──────────────────────────────┐
                    │   info      no requiere nada │
                    ├──────────────────────────────┤
                    │   warning   observa          │
                    ├──────────────────────────────┤
                    │   critical  actúa o escala   │
                    └──────────────────────────────┘
```

### INFO - Registro solamente

| # | Condición | Acción sistema | Acción persona |
| --- | --- | --- | --- |
| I-01 | Cambio de modo RED ↔ GENERADOR | Log + telemetría | Ninguna |
| I-02 | Arranque/parada normal de bomba | Log + telemetría | Ninguna |
| I-03 | Actualización de nivel dentro de rango | Log periódico | Ninguna |
| I-04 | Heartbeat MCU recibido | Contador | Ninguna |
| I-05 | Comando remoto recibido y aplicado | Audit log | Ninguna |

**Retención:** 30 días en SQLite (`metrics`). Aplica `AlertManager.purge()`.
**Notificación:** ninguna. Solo visible en el dashboard.

---

### WARNING - Observar, actuar solo si se repite

| # | Condición | Acción sistema | Acción persona |
| --- | --- | --- | --- |
| W-01 | Corriente > 40 A (aviso) | Log + alerta Telegram | Ninguna |
| W-02 | Nivel > 90 % (alto) | Alerta Telegram | Ninguna |
| W-03 | Nivel < 10 % (bajo) | Alerta Telegram | Ninguna |
| W-04 | Comando remoto **denegado** por RBAC | Audit log + alerta | Ninguna |
| W-05 | Sin heartbeat Linux por > 10 s | Reintento con backoff (1-30 s) | Ninguna |
| W-06 | PLC ordena bomba bloqueada (GENERADOR) | Log + contador | Ninguna |
| W-07 | Reintento de trip n.º 2 de 3 | Alerta Telegram | Ninguna |
| W-08 | Trip recuperado solo (0x6020) | Log + telemetría (info) | Ninguna |

**Clave:** W-07 es el aviso de que viene algo. Si llega W-07, alguien
**debería** mirar el panel antes de las 03:00, pero no está obligado.

**Retención:** 90 días. Aplica `AlertManager.purge()`.
**Canal:** Telegram (grupo de operación).

---

### CRITICAL - El sistema ya actuó o no puede actuar solo

| # | Condición | Acción sistema | Acción persona |
| --- | --- | --- | --- |
| C-01 | Trip por sobrecorriente, intentos 1-2 | Relés abiertos, re-arm tras 60 s | Ninguna (warning) |
| C-02 | Trip por sobrecorriente, **intento 3 de 3** | Relés abiertos + latched | **Sí — resetear** |
| C-03 | Contactor pegado (SIF-03) | **Emergencia global**: todos los relés abiertos | Sí si persiste |
| C-04 | Sensor de corriente (CT) fuera de rango (0x6003) | Solo alerta: **no abre relés** (no es SIF) | **Sí — verificar CT** |
| C-05 | Fault de feedback (SIF-02) | Bloquea **esa bomba**; no abre las demás | **Sí — verificar aux** |
| C-06 | **E-Stop físico activado** | PNOZ abre relés | **Sí — POR DEFINICIÓN** |
| C-07 | Heartbeat PNOZ perdido (MCU colgado) | PNOZ abre relés | **Sí — reiniciar MCU** |
| C-08 | Sin telemetría > 5 min | Alerta a canal alternativo | Sí — verificar red |
| C-09 | Estado EMERGENCIA > 30 min | Recordatorio Telegram | Sí — evaluar |
| C-10 | MANTENIMIENTO activo > 8 h | Recordatorio Telegram | Sí — cerrar o extender |
| C-11 | SIF-05: sensor de nivel fuera de rango (0x6004) | Alerta crítica + lectura marcada no confiable. **NO para las bombas** | Sí — verificar sensor |

**Retención:** 5 años (registros de seguridad). `purge()` los borra al
cumplir; antes no existía ningún `DELETE` en el proyecto.
**Canal:** Telegram + escalamiento según `escalation_matrix.md`.

---

## Matriz de decisión: ¿qué severity?

```text
¿El sistema se auto-recuperó solo?
├── NO → ¿Requiere RESET manual?
│        ├── SÍ  → CRITICAL
│        └── NO  → WARNING
└── SÍ → ¿Es información o tendencia?
         ├── Tendencia → WARNING
         └── Dato     → INFO
```

> **Excepción que rompe la regla:** C-06 (E-Stop) y C-07 (MCU colgado) son
> CRITICAL aunque el sistema ya se haya parado. Son los dos casos donde una
> persona **tiene** que ir, sin importar la hora.

---

## Referencias cruzadas

| Documento | Para qué |
| --- | --- |
| `docs/escalation_matrix.md` | Quién atiende cada nivel y en cuánto tiempo |
| `docs/playbooks.md` | Pasos concretos por escenario |
| `docs/incident_response.md` | Declaración, comunicación, recuperación, revisión |
| `docs/drills.md` | Simulacros para verificar que lo anterior funciona |
| `docs/security_protocols.md` | SIF y proof tests asociados |
| `docs/hardware_spec.md` | PT-01..PT-15 (PT-10..PT-15 solo con bootloader A/B) |

---

Fase 5 — Documento 2 de 5
