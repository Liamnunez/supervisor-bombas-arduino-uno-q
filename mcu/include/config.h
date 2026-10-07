#ifndef CONFIG_H
#define CONFIG_H

#pragma once

// ============================================================
// CONFIGURACIÓN DE HARDWARE - SUPERVISOR DE BOMBAS
// ============================================================

// --- Pines MCU (STM32U585) ---
// Entradas desde PLC (señales de arranque bombas)
#define PIN_PLC_BOMBA1      PA0   // Entrada digital - PLC ordena Bomba 1
#define PIN_PLC_BOMBA2      PA1   // Entrada digital - PLC ordena Bomba 2
#define PIN_PLC_BOMBA3      PA2   // Entrada digital - PLC ordena Bomba 3

// Salidas a relés intermedios (controlan bobinas contactores)
#define PIN_RELE_BOMBA1     PB0   // Salida digital - Relé Bomba 1
#define PIN_RELE_BOMBA2     PB1   // Salida digital - Relé Bomba 2
#define PIN_RELE_BOMBA3     PB2   // Salida digital - Relé Bomba 3

// Detección modo (contacto seco inversor)
#define PIN_MODO_GEN        PC0   // 1=RED, 0=GENERADOR (pull-up interno)

// Retornos auxiliares contactores (estado real)
#define PIN_FEEDBACK_B1     PC1   // Retorno aux Bomba 1 (NO)
#define PIN_FEEDBACK_B2     PC2   // Retorno aux Bomba 2 (NO)
#define PIN_FEEDBACK_B3     PC3   // Retorno aux Bomba 3 (NO)

// Nivel de agua (4-20mA -> ADC)
#define PIN_NIVEL_ADC       PA4   // ADC1_IN9 - Sensor 4-20mA (con divisor)

// Corriente generador (propio - NO toca el CT del DSE 7320)
// Pinza/CT en serie con la salida del generador, front-end analógico
// con rectificación + filtro -> voltaje DC proporcional a RMS
#define PIN_CORRIENTE_ADC   PA5   // ADC - Voltaje DC proporcional a corriente RMS

// Comunicación MCU <-> Linux (QRB2210)
#define PIN_UART_TX         PA9   // UART1_TX -> Linux RX
#define PIN_UART_RX         PA10  // UART1_RX -> Linux TX
#define UART_BAUDRATE       115200

// LED indicadores onboard
#define PIN_LED_OK          PB13  // Verde - Sistema OK
#define PIN_LED_GEN         PB14  // Amarillo - Modo Generador
#define PIN_LED_FAULT       PB15  // Rojo - Fallo

// Latido al relé de seguridad Pilz PNOZ s4 (entradas P1/P2).
// Modo watchdog: si el pulso se interrumpe, el PNOZ abre 13-14 y 23-24
// -> los relés de bomba quedan sin alimentación -> 0 bombas. Así el E-Stop
// y el fallo del MCU no dependen del firmware.
// ⚠️ Verificar que PB3 esté expuesto en el conector del UNO Q antes de
//    cablear; alternativas libres a confirmar: PB12, PC4.
#define PIN_PNOZ_HEARTBEAT  PB3   // Salida digital - Latido PNOZ s4 (reposo LOW)

// --- Parámetros Eléctricos ---
#define RELAY_ACTIVE_LEVEL  HIGH  // Relés: HIGH = cerrado (paso señal)
#define RELAY_INACTIVE_LEVEL LOW  // Relés: LOW = abierto (bloqueo)
#define FEEDBACK_ACTIVE_LEVEL HIGH  // Retorno aux: HIGH = contactor cerrado

// Tiempos (ms)
#define DEBOUNCE_MS         50    // Anti-rebote entradas digitales
#define FEEDBACK_TIMEOUT_MS 2000  // Timeout espera retorno aux tras orden
#define HEARTBEAT_MS        1000  // Latido MCU -> Linux
#define WATCHDOG_TIMEOUT_MS 5000  // Watchdog interno

// Latido PNOZ s4: periodo entre pulsos. El pulso dura 1 ciclo del loop
// de 100 Hz (10 ms). Ajustar al manual del PNOZ s4 y verificar en banco.
#define PNOZ_PULSE_PERIOD_MS 100
// Si el bloque de 100Hz se retrasa más que esto (bloqueo del loop), se
// fuerza el pin a LOW para que el PNOZ detecte ausencia de pulso en vez de
// ver un HIGH colgado.
#define PNOZ_STALE_MS        50

// --- Actualización de firmware (OTA firmado, Fase 4) ---
// Ver docs/ota_procedure.md. Estado real: la lógica de verificación de
// imagen y los slots A/B con rollback están implementados y testeados
// nativamente (tests/native/test_fw_image.cpp, test_boot_slot.cpp), pero
// NO hay backend criptográfico Ed25519 ni particiones A/B en flash. Mientras
// eso no exista, fwVerifyImage() rechaza TODA imagen: fail-closed.
#define FW_OTA_ENABLED         false    // no montar el flujo OTA sin bootloader
#define OTA_CONFIRM_TIMEOUT_MS 60000    // 60 s para auto-testear y confirmar
#define OTA_SLOT_MAX_BYTES     524288   // 512 KB/slot - AJUSTAR al mapa real
#define FW_CURRENT_VERSION     1        // versión del firmware en ejecución

// Nivel de agua (ADC 12-bit: 0-4095)
// Sensor 4-20mA con resistor 120ohm -> 0.48V-2.4V -> ADC ~780-3900
#define NIVEL_ADC_MIN       800   // ~4mA (0% / vacío)
#define NIVEL_ADC_MAX       3900  // ~20mA (100% / lleno)
#define NIVEL_CRITICO_BAJO  10    // % - Alarma nivel bajo
#define NIVEL_CRITICO_ALTO  90    // % - Alarma nivel alto

// Número de bombas
#define NUM_BOMBAS          3

// ============================================================
// PRESUPUESTO ELÉCTRICO Y PROTECCIÓN DE CORRIENTE
// ============================================================
// Bomba: ~30A c/u. Generador emergencia: ~45A.
// => En GENERADOR solo cabe UNA bomba (30A); 2 bombas = 60A = apagón
//    del DSE 7320 (su protección de sobrecarga corta todo).
// El Arduino vigila la corriente con su PROPIO CT (no se modifica el
// del DSE) y actúa ANTES de que el DSE llegue a su punto de corte.

// Calibración CT (front-end analógico propio - calibrar con pinza amperométrica)
#define CT_FULL_SCALE_A     100.0f   // Corriente que corresponde a CT_V_MAX
#define CT_V_MAX            3.3f     // Voltaje de full-scale del ADC

// Umbrales (A)
#define GEN_CAPACIDAD_A     45.0f    // Capacidad generador
#define CORRIENTE_AVISO_A   40.0f    // Aviso: cerca del límite
#define CORRIENTE_TRIP_A    42.0f    // Trip: actuar ANTES del corte del DSE
#define CORRIENTE_RESET_A   37.0f    // Histeresis: condiciones normales de nuevo
#define CONTACTOR_PEGADO_A  2.0f     // Corriente con relés abiertos = contactor pegado
#define CT_FAULT_MAX_A      120.0f   // Fuera de rango del CT -> sensor inválido

// Tiempos (ms)
#define CORRIENTE_TRIP_DELAY_MS    3000  // > arranque de motor (~2s inrush) - time-overcurrent
#define CORRIENTE_AVISO_DELAY_MS   1000
#define CORRIENTE_RESET_DELAY_MS   2000  // Condición limpia antes de re-armar
#define CONTACTOR_PEGADO_DELAY_MS  1000
#define CT_FAULT_DELAY_MS          500

// --- Política de trip por sobrecorriente (SIF-04) - acordada en Fase 5 ---
// El generador NO soporta 2 bombas. Si el trip deja el sistema parado
// hasta que llegue un operador, en una tormenta nocturna el pozo se
// desborda. Objetivo: recuperar solo sin abandonar al generador.
//
//   trip -> abre todos los relés (protege al DSE) -> espera enfriamiento
//         -> si la corriente está limpia, re-arma solo
//   TRIP_MAX_ATTEMPTS trips dentro de TRIP_WINDOW_MS -> NO re-arma más,
//         queda latcheado y espera a una persona.
//
// El enfriamiento de 60 s existe para no machacar los motores con
// arranques repetidos, no para proteger al generador (de eso ya se
// encarga el hecho de abrir los relés).
#define TRIP_MAX_ATTEMPTS     3         // intentos antes de exigir operador
#define TRIP_WINDOW_MS        900000    // 15 min - ventana de conteo
#define TRIP_COOLDOWN_MS      60000     // 60 s entre reintentos
#define TRIP_RECOVER_AMPS     5.0f      // corriente máx. para re-armar

// Códigos de error 0x60xx (corriente / protección)
#define ERR_SOBRECARGA       0x6001  // Trip sobrecarga generador
#define ERR_CONTACTOR_PEGADO 0x6002  // Corriente con relés abiertos
#define ERR_SENSOR_CORRIENTE 0x6003  // CT fuera de rango / desconectado
#define ERR_AVISO_SOBRECARGA 0x6010  // Aviso proximidad al límite
#define ERR_TRIP_AUTO_RECOVER 0x6020  // Re-arm automático tras trip
#define ERR_TRIP_LATCH       0x6021  // Intentos agotados: exige operador

// --- Estados del Sistema ---
enum class SystemState : uint8_t {
    NORMAL = 0,       // Modo RED - 3 bombas permitidas
    GENERADOR = 1,    // Modo GEN - Solo Bomba 1
    EMERGENCIA = 2,   // Fallo crítico - 0 bombas
    MANTENIMIENTO = 3 // Modo local - 0 bombas
};

// --- Comandos MCU -> Linux ---
enum class McuEvent : uint8_t {
    STATE_CHANGE = 0x10,
    BOMBA_START = 0x20,
    BOMBA_STOP = 0x21,
    BOMBA_FAULT = 0x22,
    FEEDBACK_MISMATCH = 0x23,
    NIVEL_UPDATE = 0x30,
    CORRIENTE_UPDATE = 0x31,
    MODO_CHANGE = 0x40,
    HEARTBEAT = 0x50,
    ERROR = 0xFF
};

// --- Estructura de mensaje MCU-Linux ---
#pragma pack(push, 1)
struct McuMessage {
    uint8_t start_byte;     // 0xAA
    uint8_t msg_type;       // McuEvent
    uint8_t bomba_id;       // 0-2 (0xFF = N/A)
    uint32_t timestamp;     // ms desde boot
    uint16_t payload;       // Dato extra (nivel %, error code, etc)
    uint8_t crc8;           // CRC8 del mensaje
    uint8_t end_byte;       // 0x55
};
#pragma pack(pop)

// CRC8 polinomio 0x07 (Dallas/Maxim)
static inline uint8_t crc8(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (crc << 1) ^ 0x07 : (crc << 1);
        }
    }
    return crc;
}

// Validar mensaje
static inline bool validar_mensaje(const McuMessage& msg) {
    uint8_t calc_crc = crc8((uint8_t*)&msg, sizeof(McuMessage) - 2);
    return msg.start_byte == 0xAA && msg.end_byte == 0x55 && calc_crc == msg.crc8;
}

#endif // CONFIG_H