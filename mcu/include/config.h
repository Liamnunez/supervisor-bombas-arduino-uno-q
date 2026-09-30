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

// Comunicación MCU <-> Linux (QRB2210)
#define PIN_UART_TX         PA9   // UART1_TX -> Linux RX
#define PIN_UART_RX         PA10  // UART1_RX -> Linux TX
#define UART_BAUDRATE       115200

// LED indicadores onboard
#define PIN_LED_OK          PB13  // Verde - Sistema OK
#define PIN_LED_GEN         PB14  // Amarillo - Modo Generador
#define PIN_LED_FAULT       PB15  // Rojo - Fallo

// --- Parámetros Eléctricos ---
#define RELAY_ACTIVE_LEVEL  HIGH  // Relés: HIGH = cerrado (paso señal)
#define RELAY_INACTIVE_LEVEL LOW  // Relés: LOW = abierto (bloqueo)
#define FEEDBACK_ACTIVE_LEVEL HIGH  // Retorno aux: HIGH = contactor cerrado

// Tiempos (ms)
#define DEBOUNCE_MS         50    // Anti-rebote entradas digitales
#define FEEDBACK_TIMEOUT_MS 2000  // Timeout espera retorno aux tras orden
#define HEARTBEAT_MS        1000  // Latido MCU -> Linux
#define WATCHDOG_TIMEOUT_MS 5000  // Watchdog interno

// Nivel de agua (ADC 12-bit: 0-4095)
// Sensor 4-20mA con resistor 120ohm -> 0.48V-2.4V -> ADC ~780-3900
#define NIVEL_ADC_MIN       800   // ~4mA (0% / vacío)
#define NIVEL_ADC_MAX       3900  // ~20mA (100% / lleno)
#define NIVEL_CRITICO_BAJO  10    // % - Alarma nivel bajo
#define NIVEL_CRITICO_ALTO  90    // % - Alarma nivel alto

// Número de bombas
#define NUM_BOMBAS          3

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