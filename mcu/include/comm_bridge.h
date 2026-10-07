/**
 * @file comm_bridge.h
 * @brief Comunicación MCU <-> Linux (QRB2210) via UART
 * 
 * Protocolo: Mensajes binarios con CRC8, framing 0xAA...0x55
 * UART1: PA9(TX) / PA10(RX) @ 115200 8N1
 */

#ifndef COMM_BRIDGE_H
#define COMM_BRIDGE_H

#include "config.h"
#include "state_machine.h"
#include "trip_policy.h"

class CommBridge {
public:
    CommBridge();
    
    void begin();
    void update(const StateMachine& sm, uint8_t nivel_pct);
    // Procesa comandos recibidos de Linux.
    // @param policy política de trip: el reset de operador debe limpiar sus
    //        intentos, o el siguiente trip heredaría los intentos viejos.
    void processCommands(StateMachine& sm, TripPolicy& policy);
    
    // Envío de eventos
    void sendStateChange(SystemState nuevo, SystemState anterior);
    void sendBombaEvent(uint8_t bomba_id, McuEvent evento);
    void sendHeartbeat(SystemState estado, uint8_t nivel_pct, bool sensor_ok);
    void sendNivelUpdate(uint8_t nivel_pct, bool sensor_ok);
    void sendCorriente(float amps);   // Telemetría corriente generador (1Hz)
    void sendError(uint16_t codigo, const char* msg = nullptr);
    
    // Configuración
    void setRxCallback(void (*cb)(const McuMessage&)) { rx_callback = cb; }

private:
    // Buffer RX circular
    static constexpr size_t RX_BUF_SIZE = 256;
    uint8_t rx_buffer[RX_BUF_SIZE];
    size_t rx_head = 0;
    size_t rx_tail = 0;
    
    // Buffer TX
    static constexpr size_t TX_BUF_SIZE = 128;
    uint8_t tx_buffer[TX_BUF_SIZE];
    
    // Callbacks
    void (*rx_callback)(const McuMessage&) = nullptr;
    
    // Estado
    uint32_t last_tx = 0;
    bool link_up = false;
    
    // UART
    void uartWrite(const uint8_t* data, size_t len);
    bool uartRead(uint8_t& byte);
    bool tryParseMessage(McuMessage& msg);
    void handleCommand(const McuMessage& msg, StateMachine& sm, TripPolicy& policy);
    void buildMessage(McuMessage& msg, McuEvent type, uint8_t bomba_id, uint16_t payload);
};

#endif // COMM_BRIDGE_H