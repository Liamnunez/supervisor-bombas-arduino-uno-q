/**
 * @file relay_control.cpp
 * @brief Implementación control de relés con anti-cruzamiento
 */

#include "relay_control.h"
#include <Arduino.h>

RelayControl::RelayControl() {
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        relay_states[i] = false;
        last_change[i] = 0;
    }
}

void RelayControl::begin() {
    // Pines ya configurados en setup() main.cpp (fail-safe early)
    // Confirmar estado inicial: todos abiertos
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        writeRelay(i, false);
        relay_states[i] = false;
        last_change[i] = millis();
    }
}

void RelayControl::apply(const StateMachine& sm) {
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bool desired = false;
        
        // Obtener estado deseado de la bomba desde la máquina de estados
        // Accedemos a la estructura interna (friend o método público)
        // Por ahora usamos puedeArrancar + plc_order
        if (sm.puedeArrancar(i)) {
            // En modo normal/generador (si permitida), seguir orden PLC
            // Necesitamos acceso a plc_order - por simplicidad lo consultamos
            // En implementación real, StateMachine expondría getPlcOrder()
            desired = true;  // placeholder - se corrige abajo
        }
        
        // Anti-cruzamiento: no conmutar muy rápido
        if (desired != relay_states[i]) {
            if (now - last_change[i] >= MIN_SWITCH_INTERVAL_MS) {
                writeRelay(i, desired);
                relay_states[i] = desired;
                last_change[i] = now;
            }
        }
    }
}

// Método mejorado que usa info completa del StateMachine
void RelayControl::applyDetailed(const bool plc_orders[NUM_BOMBAS], 
                                  const bool allowed[NUM_BOMBAS]) {
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bool desired = allowed[i] && plc_orders[i];
        
        if (desired != relay_states[i]) {
            if (now - last_change[i] >= MIN_SWITCH_INTERVAL_MS) {
                writeRelay(i, desired);
                relay_states[i] = desired;
                last_change[i] = now;
            }
        }
    }
}

void RelayControl::emergencyOpenAll() {
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        writeRelay(i, false);
        relay_states[i] = false;
        last_change[i] = millis();
    }
}

void RelayControl::setRelay(uint8_t bomba_id, bool closed) {
    if (bomba_id >= NUM_BOMBAS) return;
    writeRelay(bomba_id, closed);
    relay_states[bomba_id] = closed;
    last_change[bomba_id] = millis();
}

bool RelayControl::getRelayState(uint8_t bomba_id) const {
    if (bomba_id >= NUM_BOMBAS) return false;
    return relay_states[bomba_id];
}

bool RelayControl::verifyCoherence(const StateMachine& sm) const {
    // Verificar que salidas físicas coincidan con estado interno
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bool physical = digitalRead(i == 0 ? PIN_RELE_BOMBA1 : 
                                    i == 1 ? PIN_RELE_BOMBA2 : PIN_RELE_BOMBA3);
        if (physical != relay_states[i]) {
            return false;
        }
    }
    return true;
}

void RelayControl::writeRelay(uint8_t bomba_id, bool closed) {
    uint8_t pin;
    switch (bomba_id) {
        case 0: pin = PIN_RELE_BOMBA1; break;
        case 1: pin = PIN_RELE_BOMBA2; break;
        case 2: pin = PIN_RELE_BOMBA3; break;
        default: return;
    }
    digitalWrite(pin, closed ? RELAY_ACTIVE_LEVEL : RELAY_INACTIVE_LEVEL);
}