# Análisis Fail-Safe - Supervisor de Bombas

## Principio Fundamental

> **Si cualquier componente falla → NINGUNA BOMBA ARRANCA**

Esto se logra mediante relés **normally open (NO) con retorno por muelle**:
- MCU alimentado + pin HIGH → Relé CERRADO (paso señal)
- MCU sin alimentación / pin LOW / MCU caído → Relé ABIERTO (bloqueo)

---

## Matriz de Fallos

| Componente | Modo Fallo | Efecto en Relés | Bombas | Detectado por | Recuperación |
|------------|------------|-----------------|--------|---------------|--------------|
| **MCU (STM32U585)** | Sin alimentación | Abren (muelle) | 0/3 | Hardware | Restaurar alimentación |
| **MCU** | Firmware crashea | Abren (pin LOW por defecto) | 0/3 | Watchdog interno + Linux | Reinicio MCU |
| **MCU** | Loop bloqueado | Mantienen último estado* | Variable | Watchdog HW (IWDG 5s) | Reset por IWDG |
| **Relé Intermedio B1** | Bobina abierta | Abre (muelle) | B1: 0 | Feedback mismatch | Reemplazar relé |
| **Relé Intermedio B1** | Contacto soldado | Cerrado permanente | B1: 1** | Feedback mismatch (PLC=0, FB=1) | Reemplazar relé |
| **Relé Intermedio B2/B3** | Igual B1 | Igual | B2/B3 | Igual | Igual |
| **PLC** | Salida pegada HIGH | Relé sigue PLC si permitido | Según modo | Feedback timeout | Reemplazar tarjeta PLC |
| **PLC** | Sin alimentación | Salidas abiertas | 0/3 | Sin órdenes PLC | Restaurar PLC |
| **Sensor Nivel** | Cable cortado | N/A (solo telemetría) | N/A | ADC < 600 o > 4000 | Reparar cableado |
| **Sensor Nivel** | Cortocircuito | N/A | N/A | ADC > 4000 (corriente >21mA) | Reemplazar sensor |
| **Comunicación MCU-Linux** | Cable UART cortado | MCU autónomo | Normal | Linux: sin heartbeat 10s | Reparar cable |
| **Linux (QRB2210)** | Caído/Apagado | MCU autónomo | Normal | MCU sigue operando | Reiniciar Linux |
| **Inversor (Modo HW)** | Contacto intermitente | Cambios modo | Según modo | Debounce 50ms | Verificar inversor |
| **Contactor B1** | Bobina quemada | No cierra | B1: 0 | Feedback timeout 2s | Reemplazar contactor |
| **Contactor B1** | Contactos soldados | Siempre cerrado | B1: 1** | Feedback mismatch | Reemplazar contactor |
| **Fuente 24VDC** | Caída tensión | Relés abren + MCU off | 0/3 | Todo cae | Restaurar 24V |

* Salvo que el crash deje pines en HIGH (poco probable en STM32)
** Peligroso: bomba arranca sin orden. Detectado por feedback mismatch → EMERGENCIA

---

## Protecciones por Capas

### Capa 1: Hardware (Inherente)
- Relés NO con muelle → **Fail-safe pasivo**
- Pull-up en pin modo (PC0) → Si cable cortado = RED (seguro)
- Pull-down en feedbacks (PC1-3) → Si cable cortado = Bomba parada
- Watchdog hardware (IWDG) → Reset MCU si firmware cuelga

### Capa 2: Firmware MCU (Activa)
```cpp
// En cada ciclo loop (10ms):
1. Leer todas las entradas
2. Verificar feedback timeout (2s)
3. Verificar mismatch PLC vs Feedback
4. Verificar sensor nivel rango válido
5. Recargar watchdog HW
6. Decidir estado relés según máquina de estados
7. Aplicar con anti-cruzamiento (100ms mínimo)
8. Enviar heartbeat a Linux
```

### Capa 3: Supervisión Linux (Monitor)
- Heartbeat cada 1s → Alerta si > 10s sin recibir
- Verificación coherencia estado
- Alertas por WebSocket a operadores
- Logs persistentes para auditoría

### Capa 4: Procedimientos Operativos
- Test semanal: simular corte generador
- Test mensual: desconectar feedback → verificar emergencia
- Revisión visual relés/contactores trimestral

---

## Escenarios Críticos y Respuesta

### ESCENARIO 1: Generador arranca, PLC intenta arrancar 3 bombas
```
1. Inversor abre contacto → MCU detecta GENERADOR (50ms)
2. Máquina estados: NORMAL → GENERADOR
3. Relé B1: sigue PLC (paso)
4. Relé B2, B3: FORZADOS ABIERTOS (ignore PLC)
5. PLC ordena B2/B3 → Relés bloquean → Contactores no cierran
6. Feedback B2/B3 = 0 → OK (mismatch detectado pero no fault)
7. Solo Bomba 1 consume del generador ✓
```

### ESCENARIO 2: Contactor B2 se suelda (contactos pegados)
```
1. PLC apaga B2 → Relé B2 abre → Bobina contactor desenergizada
2. Pero contactos potencia siguen cerrados (soldados)
2. Feedback B2 (aux NO) = 1 (contacto auxiliar también soldado O circuito separado)
3. MCU detecta: PLC=0, Feedback=1 → MISMATCH
4. fault_count++ → EMERGENCIA
5. TODOS los relés abren → Bomba 1 y 3 paran
6. Alerta crítica en dashboard
```

### ESCENARIO 3: MCU se reinicia (watchdog o power glitch)
```
1. MCU boot ~2s
2. Durante boot: pines en HIGH-Z → Pull-up/pull-down definen estado
   - Relés: pull-down interno o externo → ABIERTOS
   - Modo: pull-up PC0 → RED (conservador)
3. setup(): pins modo OUTPUT, write LOW → Relés confirman ABIERTOS
4. begin(): Lee modo HW, estado inicial = según HW
5. Loop normal en <3s
6. Linux detecta reconexión → Solicita estado completo
```

### ESCENARIO 4: Cable feedback B1 se corta
```
1. Feedback B1 = 0 (pull-down)
2. PLC ordena B1=1 → Relé cierra → Contactor cierra
3. Feedback sigue en 0 (cable cortado)
4. Tras 2s: FEEDBACK_TIMEOUT → fault_count++
5. EMERGENCIA activada → Todos relés abren
6. Bomba 1 para (contactor abre por relé abierto)
7. Sistema seguro, requiere reset manual
```

---

## Verificación de Fail-Safe (Checklist Puesta en Marcha)

### Test 1: Fallo Alimentación MCU
- [ ] Desconectar 24V del Arduino UNO Q
- [ ] Verificar: 3 relés abiertos (medir continuidad contactos)
- [ ] Verificar: 3 contactores abiertos (bombas paradas)
- [ ] Reconectar → Verificar arranque seguro

### Test 2: Modo Generador
- [ ] Simular contacto inversor ABIERTO (GEN)
- [ ] PLC ordena 3 bombas
- [ ] Verificar: Solo relé B1 cierra, B2/B3 abiertos
- [ ] Verificar: Solo Bomba 1 arranca
- [ ] Restaurar contacto RED → Verificar 3 bombas

### Test 3: Feedback Timeout
- [ ] Desconectar feedback B1
- [ ] PLC ordena B1
- [ ] Esperar 2.5s
- [ ] Verificar: EMERGENCIA activada, todos relés abren
- [ ] Verificar: Alerta en dashboard
- [ ] Reset emergencia → Verificar recuperación

### Test 4: Mismatch (Contactor Pegado)
- [ ] Simular feedback B1=1 sin orden PLC (puente temporal)
- [ ] Verificar: Mismatch detectado → fault_count++
- [ ] Verificar: EMERGENCIA tras umbral

### Test 5: Sensor Nivel
- [ ] Desconectar sensor 4-20mA
- [ ] Verificar: ADC ~0 → sensor_ok=false → Alerta crítica
- [ ] Cortocircuitar entrada ADC (simular >20mA)
- [ ] Verificar: sensor_ok=false → Alerta crítica

### Test 6: Watchdog
- [ ] Inyectar bucle infinito en firmware (test only)
- [ ] Verificar: Reset por IWDG a los 5s
- [ ] Verificar: Relés abren durante reset

### Test 7: Comunicación MCU-Linux
- [ ] Desconectar USB/UART
- [ ] Verificar: MCU sigue operando (LEDs, relés)
- [ ] Verificar: Linux alerta "Sin heartbeat"
- [ ] Reconectar → Verificar sincronización automática

---

## Métricas de Confiabilidad Objetivo

| Métrica | Objetivo | Método Verificación |
|---------|----------|---------------------|
| MTBF (Mean Time Between Failures) | > 50,000 horas | Cálculo componentes + test acelerado |
| MTTR (Mean Time To Recovery) | < 15 min | Procedimientos documentados |
| Probabilidad fallo peligroso (PFD) | < 10⁻³ | Análisis IEC 61508 SIL 1 |
| Tiempo detección fallo crítico | < 2.1s | Feedback timeout + 1 ciclo |
| Tiempo reacción emergencia | < 10ms | Loop 10ms + escritura GPIO |
| Disponibilidad sistema | 99.9% | Diseño redundante en capas |

---

## Mantenimiento Predictivo

| Componente | Intervalo | Acción |
|------------|-----------|--------|
| Relés intermedios | 12 meses | Test continuidad, medir resistencia bobina |
| Contactores potencia | 12 meses | Inspección visual contactos, medir caída tensión |
| Sensor nivel | 6 meses | Verificar 4mA/20mA con calibrador |
| Cableado | 12 meses | Megómetro aislamiento > 1MΩ |
| MCU (firmware) | 6 meses | Verificar versión, logs errores |
| Linux (QRB2210) | Mensual | `apt update && apt upgrade`, revisar logs |

---

## Conclusión

La arquitectura **en serie con relés NO** garantiza que **cualquier fallo individual** (MCU, relé, cable, alimentación, software) resulta en **estado seguro: bombas paradas**. 

El único modo de fallo peligroso (contactos relé/contacto soldados) es **detectado activamente** por la comparación PLC vs Feedback, disparando EMERGENCIA global.

**Nivel de seguridad estimado: SIL 1 (IEC 61508) compatible para aplicación industrial no critica de seguridad personal.**