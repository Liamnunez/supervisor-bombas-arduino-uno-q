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
    // Leer bytes del UART HARDWARE y meterlos en el anillo interno.
    //
    // Antes este bucle llamaba a uartRead(), que ya lee del anillo
    // (rx_tail..rx_head). Como nada copiaba del UART real al anillo, un
    // byte pendiente en el hardware dejaba available() a true para
    // siempre, uartRead() devolvía false y el bucle no terminaba nunca:
    // el firmware se colgaba con el primer byte recibido.
    int presupuesto = RX_BUF_SIZE;   // tope: no se debe bloquear el loop
    while (MCU_SERIAL.available() && presupuesto-- > 0) {
        int v = MCU_SERIAL.read();
        if (v < 0) break;
        rx_buffer[rx_head] = static_cast<uint8_t>(v);
        rx_head = (rx_head + 1) % RX_BUF_SIZE;
        // Ring lleno: descartar el más antiguo en vez de sobrescribir a
        // medias. Un frame corrupto es mejor que perderlos todos.
        if (rx_head == rx_tail) {
            rx_tail = (rx_tail + 1) % RX_BUF_SIZE;
            overflow_count++;
        }
    }

    // NO se parsea nada aquí. processCommands() es el único consumidor del
    // anillo: si update() tambien consumiera, se llevara los frames antes
    // de que processCommands() los viera y TODOS los comandos remotos se
    // perderian en silencio (main.cpp no fija setRxCallback, asi que no
    // habia ningun otro consumidor). El comentario original de aqui ya
    // decia "Los comandos se procesan en processCommands()", pero el
    // codigo no lo cumplia.
    (void)sm;
    (void)nivel_pct;
}

void CommBridge::processCommands(StateMachine& sm, TripPolicy& policy) {
    // Único consumidor del anillo: parsea y despacha. Se llama en cada
    // vuelta del loop principal (main.cpp), también fuera del bloque de
    // 100 Hz, para que un comando no espere al siguiente tick.
    McuMessage msg;
    while (tryParseMessage(msg)) {
        if (rx_callback) rx_callback(msg);
        handleCommand(msg, sm, policy);
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

void CommBridge::sendHeartbeat(SystemState estado, uint8_t nivel_pct, bool sensor_ok) {
    McuMessage msg;
    uint16_t payload = (static_cast<uint16_t>(estado) << 8) | (nivel_pct & 0x7F) | (sensor_ok ? 0x80 : 0x00);
    buildMessage(msg, McuEvent::HEARTBEAT, 0xFF, payload);
    uartWrite((uint8_t*)&msg, sizeof(McuMessage));
}

void CommBridge::sendNivelUpdate(uint8_t nivel_pct, bool sensor_ok) {
    McuMessage msg;
    uint16_t payload = (nivel_pct & 0x7F) | (sensor_ok ? 0x80 : 0x00);
    buildMessage(msg, McuEvent::NIVEL_UPDATE, 0xFF, payload);
    uartWrite((uint8_t*)&msg, sizeof(McuMessage));
}

void CommBridge::sendCorriente(float amps) {
    if (amps < 0) amps = 0;
    uint16_t deciamps = static_cast<uint16_t>(amps * 10.0f);  // 0.1A de resolución
    McuMessage msg;
    buildMessage(msg, McuEvent::CORRIENTE_UPDATE, 0xFF, deciamps);
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
        rx_rejected_count++;
        return false;
    }
    
    // Mensaje válido: avanzar tail
    rx_tail = (start + sizeof(McuMessage)) % RX_BUF_SIZE;
    rx_ok_count++;
    return true;
}

void CommBridge::handleCommand(const McuMessage& msg, StateMachine& sm, TripPolicy& policy) {
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
            // Reset de emergencia + latch de fallos (comando operador)
            if (msg.payload == 0xFFFF) {
                sm.resetEmergencia();
                sm.setMantenimiento(false);
                // El operador ha atendido: la política de trip
                // vuelve a empezar. Sin esto, el siguiente trip heredaría
                // los intentos anteriores y podría latchear de inmediato.
                policy.operatorReset();
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