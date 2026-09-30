/**
 * @file comm_bridge.cpp
 * @brief Implementación comunicación MCU-Linux
 */

#include "comm_bridge.h"
#include <Arduino.h>

// Usar Serial1 para UART1 (PA9/PA10) en STM32U585
#define MCU_SERIAL Serial1

CommBridge::CommBridge() {}

void CommBridge::begin() {
    MCU_SERIAL.begin(UART_BAUDRATE);
    while (!MCU_SERIAL && millis() < 100);  // Brief wait
    
    // Limpiar buffers
    rx_head = rx_tail = 0;
    link_up = true;
}

void CommBridge::update(const StateMachine& sm, uint8_t nivel_pct) {
    // Leer bytes disponibles
    while (MCU_SERIAL.available()) {
        uint8_t byte;
        if (uartRead(byte)) {
            rx_buffer[rx_head] = byte;
            rx_head = (rx_head + 1) % RX_BUF_SIZE;
        }
    }
    
    // Intentar parsear mensajes completos
    McuMessage msg;
    while (tryParseMessage(msg)) {
        if (rx_callback) rx_callback(msg);
        // Los comandos se procesan en processCommands()
    }
}

void CommBridge::processCommands(StateMachine& sm) {
    // Procesar comandos pendientes en el buffer
    McuMessage msg;
    while (tryParseMessage(msg)) {
        handleCommand(msg, sm);
    }
}

void CommBridge::sendStateChange(SystemState nuevo, SystemState anterior) {
    McuMessage msg;
    buildMessage(msg, McuEvent::STATE_CHANGE, 0xFF, 
                 (static_cast<uint16_t>(anterior) << 8) | static_cast<uint16_t>(nuevo));
    uartWrite((uint8_t*)&msg, sizeof(McuMessage));
}

void CommBridge::sendBombaEvent(uint8_t bomba_id, McuEvent evento) {
    McuMessage msg;
    buildMessage(msg, evento, bomba_id, 0);
    uartWrite((uint8_t*)&msg, sizeof(McuMessage));
}

void CommBridge::sendHeartbeat(SystemState estado, uint8_t nivel_pct) {
    McuMessage msg;
    buildMessage(msg, McuEvent::HEARTBEAT, 0xFF, 
                 (static_cast<uint16_t>(estado) << 8) | nivel_pct);
    uartWrite((uint8_t*)&msg, sizeof(McuMessage));
}

void CommBridge::sendNivelUpdate(uint8_t nivel_pct) {
    McuMessage msg;
    buildMessage(msg, McuEvent::NIVEL_UPDATE, 0xFF, nivel_pct);
    uartWrite((uint8_t*)&msg, sizeof(McuMessage));
}

void CommBridge::sendError(uint16_t codigo, const char* msg) {
    McuMessage m;
    buildMessage(m, McuEvent::ERROR, 0xFF, codigo);
    uartWrite((uint8_t*)&m, sizeof(McuMessage));
}

void CommBridge::uartWrite(const uint8_t* data, size_t len) {
    MCU_SERIAL.write(data, len);
    MCU_SERIAL.flush();
}

bool CommBridge::uartRead(uint8_t& byte) {
    if (rx_tail != rx_head) {
        byte = rx_buffer[rx_tail];
        rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
        return true;
    }
    return false;
}

bool CommBridge::tryParseMessage(McuMessage& msg) {
    // Buscar start byte 0xAA
    size_t start = rx_tail;
    while (start != rx_head) {
        if (rx_buffer[start] == 0xAA) break;
        start = (start + 1) % RX_BUF_SIZE;
    }
    
    if (start == rx_head) return false;  // No start byte
    
    // Verificar si tenemos mensaje completo (12 bytes)
    size_t available = (rx_head >= start) ? (rx_head - start) : (RX_BUF_SIZE - start + rx_head);
    if (available < sizeof(McuMessage)) return false;  // Incompleto
    
    // Copiar mensaje
    uint8_t* dst = (uint8_t*)&msg;
    for (size_t i = 0; i < sizeof(McuMessage); i++) {
        size_t idx = (start + i) % RX_BUF_SIZE;
        dst[i] = rx_buffer[idx];
    }
    
    // Validar
    if (!validar_mensaje(msg)) {
        // Descartar byte de inicio y reintentar
        rx_tail = (start + 1) % RX_BUF_SIZE;
        return false;
    }
    
    // Mensaje válido: avanzar tail
    rx_tail = (start + sizeof(McuMessage)) % RX_BUF_SIZE;
    return true;
}

void CommBridge::handleCommand(const McuMessage& msg, StateMachine& sm) {
    switch (static_cast<McuEvent>(msg.msg_type)) {
        case McuEvent::MODO_CHANGE: {
            // Payload: bit 0 = modo generador (1=GEN, 0=RED)
            bool gen = msg.payload & 0x01;
            sm.setModoGenerador(gen);
            break;
        }
        case McuEvent::STATE_CHANGE: {
            // Comando: forzar estado
            uint8_t state = msg.payload & 0xFF;
            if (state == static_cast<uint8_t>(SystemState::EMERGENCIA)) {
                sm.triggerEmergencia(msg.payload >> 8);
            } else if (state == static_cast<uint8_t>(SystemState::MANTENIMIENTO)) {
                sm.setMantenimiento(true);
            }
            break;
        }
        case McuEvent::ERROR: {
            // Reset de emergencia
            if (msg.payload == 0xFFFF) {
                sm.setMantenimiento(false);
                // Si no hay emergencia HW, vuelve a normal/generador
            }
            break;
        }
        default:
            break;
    }
}

void CommBridge::buildMessage(McuMessage& msg, McuEvent type, uint8_t bomba_id, uint16_t payload) {
    msg.start_byte = 0xAA;
    msg.msg_type = static_cast<uint8_t>(type);
    msg.bomba_id = bomba_id;
    msg.timestamp = millis();
    msg.payload = payload;
    msg.end_byte = 0x55;
    
    // Calcular CRC8 (excluye start_byte, crc8, end_byte)
    msg.crc8 = crc8((uint8_t*)&msg + 1, sizeof(McuMessage) - 3);
}