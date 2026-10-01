/**
 * @file relay_control.h
 * @brief Control de 3 relés intermedios con anti-cruzamiento y fail-safe
 */

#ifndef RELAY_CONTROL_H
#define RELAY_CONTROL_H

#include "config.h"
#include "state_machine.h"

class RelayControl {
public:
    RelayControl();
    
    void begin();
    
    // Aplicar estado de relés según decisión de la máquina de estados
    void apply(const StateMachine& sm);
    
    // Aplicar con arrays explícitos (útil para tests / simulación)
    void applyDetailed(const bool plc_orders[NUM_BOMBAS],
                       const bool allowed[NUM_BOMBAS]);
    
    // Forzar todos los relés abiertos (emergencia)
    void emergencyOpenAll();
    
    // Forzar relé específico (para test/mantenimiento)
    void setRelay(uint8_t bomba_id, bool closed);
    
    // Leer estado actual de salidas
    bool getRelayState(uint8_t bomba_id) const;
    
    // Verificar coherencia: salida coincide con estado interno
    bool verifyCoherence(const StateMachine& sm) const;

private:
    bool relay_states[NUM_BOMBAS];
    uint32_t last_change[NUM_BOMBAS];
    
    // Anti-cruzamiento: evitar transiciones muy rápidas
    static constexpr uint32_t MIN_SWITCH_INTERVAL_MS = 100;
    
    void writeRelay(uint8_t bomba_id, bool closed);
};

#endif // RELAY_CONTROL_H