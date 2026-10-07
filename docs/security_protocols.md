# Protocolos de Seguridad - Supervisor de Bombas

## Versión 1.0 - Documento Vivo (Actualizable)

---

## 1. Alcance y Definiciones

| Término | Definición |
| --- | --- |
| **SIS** | Safety Instrumented System - Sistema Instrumentado de Seguridad |
| **SIL** | Safety Integrity Level (1-4, IEC 61508) |
| **SIF** | Safety Instrumented Function - Función Instrumentada de Seguridad |
| **SIF-01** | Función: "Limitar a 1 bomba en modo GENERADOR" |
| **SIF-02** | Función: "Parada de emergencia por feedback timeout" |
| **SIF-03** | Función: "Parada por contactor soldado (mismatch)" |
| **SIF-04** | Función: "Parada por sobrecorriente generador" |
| **BPCS** | Basic Process Control System (el PLC existente) |
| **SIS** | Safety Instrumented System (este Arduino + relés) |
| **BPCS ≠ SIS** | El PLC controla; el Arduino supervisa y bloquea |

---

## 2. Matriz de Funciones de Seguridad (SIF)

| SIF | Descripción | Trigger | Acción | SIL Target | Proof Test |
| --- | --- | --- | --- | --- | --- |
| **SIF-01** | Limitar bombas en GEN | Modo HW = GEN + PLC ordena B2/B3 | Bloquear relés B2/B3 | SIL 1 | Anual |
| **SIF-02** | Feedback timeout (no arranca) | PLC=ON, FB=OFF > 2s | Bloquear esa bomba (fault_count++) | SIL 1 | Semestral |
| **SIF-03** | Contactor soldado | PLC=OFF, FB=ON > 2s | EMERGENCIA global | SIL 1 | Semestral |
| **SIF-04** | Sobrecorriente gen | I > 42A > 3s | EMERGENCIA global | SIL 1 | Semestral |
| **SIF-05** | Nivel sensor fail | ADC < 3.5mA o > 21mA | Alerta + safe state | SIL 1 | Anual |
| **SIF-06** | Watchdog de latido HW | Pulso PB3→PNOZ P1 se interrumpe | PNOZ abre 13-14 y 23-24 | SIL 1 | Trimestral |

> **SIF-02 vs SIF-03 no son lo mismo.** SIF-02 es "mandamos arrancar y no
> arranca": la bomba queda bloqueada y las demás siguen bajo control.
> SIF-03 es "el contactor está cerrado cuando lo mandamos abierto": esa
> bomba ya **no** la controlamos, así que la parada es global. Bloquear solo
> la bomba pegada no cerraba el bypass en GENERADOR (B1 soldada consumiendo
> 30 A + arranque de B2 = 60 A sobre un generador de 45 A).
>
> **Nota sobre SIF-03 y `autoRecoverTrip()`:** la auto-recuperación solo
> actúa sobre `ERR_SOBRECARGA`. Una emergencia por contactor soldado exige
> operador siempre.
> **SIF-06** es la única SIF cuya detección no depende del firmware: la
> ejecuta el PNOZ s4 en modo watchdog. Un firmware colgado deja de emitir
> el pulso y la cadena de relés se abre sin participación del software.
> **Nota:** SIL 1 target para pilotaje. Revisar a SIL 2 tras análisis de riesgo
> (HAZOP).

---

## 3. Arquitectura de Seguridad (Capas)

```text
┌─────────────────────────────────────────────────────────────┐
│ CAPA 4: Procedimientos Operativos                           │
│  - Checklist puesta en marcha (fail_safe.md)                │
│  - Proof tests programados                                  │
│  - Competencias operador                                    │
├─────────────────────────────────────────────────────────────┤
│ CAPA 3: Supervisión Linux (Monitor)                         │
│  - Heartbeat 1s → alerta > 10s                              │
│  - Alertas WebSocket/Telegram                               │
│  - Logs inmutables (SQLite + audit.log JSONL)               │
├─────────────────────────────────────────────────────────────┤
│ CAPA 2: Firmware MCU (SIS)                                  │
│  - Loop 100Hz / Watchdog 5s (IWDG)                          │
│  - SIF-01 a SIF-05 implementadas                            │
│  - Latido PB3→PNOZ emitido cada 100ms (SIF-06)               │
│  - Anti-cruzamiento 100ms / Debounce 50ms                   │
├─────────────────────────────────────────────────────────────┤
│ CAPA 1: Hardware (Inherente)                                │
│  - Relés NO + muelle → fail-safe pasivo                     │
│  - Pull-up PC0 (modo) / Pull-down PC1-3 (feedback)          │
│  - Relé de seguridad (Pilz PNOZ) entre MCU y contactor **   │
│  - E-stop hardwired (botón físico, cableado directo) **     │
│  - Watchdog de latido MCU→PNOZ (PB3 → P1) **               │
└─────────────────────────────────────────────────────────────┘
** = Requerido para producción (no en piloto)
```

> **SIF-06 (watchdog de latido) cierra el bucle de independencia:** con el
> PNOZ en modo watchdog, ninguna condición de fallo necesita que el
> firmware "se dé cuenta" para que las bombas paren. Sin él, el PNOZ solo
> protege el E-Stop y todo lo demás depende del software.

---

## 4. Matriz de Acceso y Comandos (RBAC)

| Acción | Local (Físico) | Remoto (Operator) | Remoto (Admin) | Notas |
| --- | --- | --- | --- | --- |
| Ver estado/alertas | ✅ | ✅ | ✅ | Solo lectura |
| Consultar histórico/CSV | ✅ | ✅ | ✅ | Export CSV |
| Ver métricas/gráficas | ✅ | ✅ | ✅ | Dashboard |
| **Reset emergencia** | ✅ (llave) | ✅ (2FA) | ✅ (2FA) | **2FA obligatorio remoto** |
| **Set mantenimiento** | ✅ (llave) | ✅ | ✅ | Operator+ |
| **Set modo GEN/RED** | ✅ (llave) | ❌ | ✅ (2FA) | **Admin + 2FA (override HW opcional)** |
| Cambiar umbrales | ✅ | ❌ | ✅ (2FA) | Admin + 2FA |
| Desactivar protecciones | ❌ | ❌ | ❌ | **NUNCA** |
| Cambiar firmware | ❌ (JTAG) | ❌ | ❌ (OTA firmado) | Solo mantenimiento |

> **Regla de oro:** El límite de 1 bomba en GENERADOR es **regla de firmware
> inmutable** en operación normal. Existe comando `set_modo_generador` (solo
> admin, 2FA) para forzar modo HW en casos excepcionales, pero el firmware
> sigue bloqueando B2/B3 si el pin HW dice GEN.
>
> ---

## 4. Matriz de Acceso Remoto (Ciberseguridad)

| Capa | Piloto (3 meses) | Producción |
| --- | --- | --- |
| **Autenticación** | HMAC token (8h) | mTLS + certificado dispositivo |
| **Transporte** | HTTPS/WSS | VPN (WireGuard) + mTLS |
| **Red** | Firewall + allowlist IP | OT VLAN segregada, deny-by-default |
| **Certificados** | Autofirmados (dev) | PKI propia / ACME interno |
| **Rotación claves** | Manual (8h TTL) | Auto (ACME, 90 días) |
| **Auditoría** | JSONL local + SQLite | SIEM centralizado (Graylog/ELK) |
| **Rate limiting** | 10 req/min / IP | 5 req/min / IP + WAF |

---

## 5. Pruebas de Validación (Proof Tests)

| Test | Frecuencia | Método | Criterio Aceptación | Responsable |
| --- | --- | --- | --- | --- |
| **PT-01** Alimentación MCU | Semestral | Desconectar 24V | 3 relés abiertos < 100ms | Operador |
| **PT-02** Modo Generador | Semestral | Simular GEN + orden 3 bombas | Solo B1 cierra, B2/B3 abiertos | Operador |
| **PT-03** Feedback timeout | Semestral | Desconectar FB + orden PLC | EMERGENCIA < 2.5s | Operador |
| **PT-04** Contactor soldado | Semestral | Puente FB sin orden | EMERGENCIA < 2.5s | Operador |
| **PT-05** Sobrecorriente | Anual | Inyectar >42A > 3s | EMERGENCIA global < 100ms | Ingeniero |
| **PT-06** E-stop físico | Semestral | Presionar botón | Todos relés abiertos < 50ms | Operador |
| **PT-07** Watchdog | Anual | Inyectar loop infinito | Reset IWDG < 5s | Ingeniero |
| **PT-08** Comunicación | Mensual | Desconectar UART | Alerta Linux > 10s | Operador |
| **PT-09** Watchdog PNOZ | Trimestral | Cortar 24V y luego solo el cable P1 | Relés abiertos por PNOZ | Ingeniero |
| **PT-10** Firma corrupta | Trimestral | Alterar 1 byte de la firma | Imagen rechazada | Ingeniero |
| **PT-11** Hash inconsistente | Trimestral | Recalcular CRC32 sin SHA-256 | Rechazada en el hash | Ingeniero |
| **PT-12** Downgrade | Trimestral | Imagen con versión anterior | Rechazada (anti-rollback) | Ingeniero |
| **PT-13** Auto-test fallido | Semestral | Firmware con umbral a 0 | Rollback automático | Ingeniero |
| **PT-14** Corte en transferencia | Semestral | Cortar 24V a mitad de descarga | Slot activo intacto | Ingeniero |
| **PT-15** Corte en activación | Semestral | Cortar 24V arrancando en TRIAL | Arranca en el slot confirmado | Ingeniero |

> **PT-10..PT-15 requieren bootloader A/B instalado.** Ver
> `docs/ota_procedure.md` §10 para el estado real de la implementación.

### Cambio de firmware y proof tests

**Cualquier** cambio de firmware invalida PT-01..PT-09 hasta que se
re-ejecuten. Un binario nuevo en un slot confirmado sin re-ejecutarlos deja
la planta sin evidencia de que la función de seguridad sigue siendo la
misma. La firma demuestra que el binario es auténtico, no que esté
probado.

> **Registro:** Cada test genera acta firmada (PDF) archivada 5 años.

---

## 6. Registro de Cambios (Changelog)

| Versión | Fecha | Autor | Cambios | Aprobado |
| --- | --- | --- | --- | --- |
| 1.0 | 2026-10-02 | [Autor] | Creación inicial - Fases 1-4 | Pendiente |
| 1.1 | TBD | - | Añadir HAZOP/LOPA resultados | - |
| 1.2 | TBD | - | SIL 2 upgrade (si aplica) | - |

---

## Próximos Pasos (Fases Pendientes)

- [ ] **Fase 2:** HAZOP/LOPA - Análisis de riesgos detallado
- [ ] **Fase 3:** Especificación Hardware (relé seguridad, E-stop, cableado)
- [ ] **Fase 4:** Especificación Firmware OTA seguro (firmas Ed25519)
- [ ] **Fase 5:** Plan de respuesta a incidentes / Matriz escalamiento
- [ ] **Fase 6:** Plan de formación operadores / Competencias

---

*Fin de Fase 1 - Base documental estable. Siguiente: Fase 2 (HAZOP/LOPA) cuando
haya tiempo.*
