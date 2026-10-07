#ifndef STATE_MACHINE_H
#define STATE_MACHINE_H

#pragma once

#include "config.h"

class StateMachine {
public:
    StateMachine();
    
    // Inicialización
    void begin();
    
    // Loop principal - llamar cada ciclo
    void update();
    
    // Obtener estado actual
    SystemState getState() const { return current_state; }
    
    // Verificar si una bomba puede arrancar
    bool puedeArrancar(uint8_t bomba_id) const;
    
    // Notificar evento de feedback (retorno aux)
    void onFeedback(uint8_t bomba_id, bool activo);
    
    // Notificar orden PLC
    void onPlcOrder(uint8_t bomba_id, bool orden);
    
    // Forzar cambio de modo (desde Linux side o botón local)
    bool setModoGenerador(bool es_generador);
    
    // Forzar estado de emergencia
    void triggerEmergencia(uint16_t codigo_error);
    
    // Reset de emergencia (comando operador) - limpia emergencia Y latch de bombas
    void resetEmergencia();
    
    // Auto-recuperación de la emergencia de sobrecarga (política Fase 5).
    // A diferencia de resetEmergencia(), NO limpia el latch de fallos de
    // bombas: si una bomba perdió el feedback debe seguir bloqueada. Solo
    // se retira la emergencia que YRIGE el protector de corriente.
    // @return true si había emergencia de sobrecarga que recuperar
    bool autoRecoverTrip();
    
    // Nº de intentos de trip acumulados en la ventana (telemetría)
    uint8_t tripIntentos() const { return trip_attempts; }
    void setTripIntentos(uint8_t n) { trip_attempts = n; }
    
    // Limpiar latch de fallos de bombas (comando operador)
    void clearFaults();
    
    // ¿Emergencia activa?
    bool hayEmergencia() const { return emergencia_activa; }
    
    // Estado real de la salida de relé de una bomba
    bool getRelayClosed(uint8_t bomba_id) const {
        return bomba_id < NUM_BOMBAS ? bombas[bomba_id].relay_closed : false;
    }
    
    // Solicitar modo mantenimiento
    void setMantenimiento(bool activo);
    
    // Callbacks para notificar cambios
    using StateChangeCallback = void (*)(SystemState nuevo, SystemState anterior);
    using BombaEventCallback = void (*)(uint8_t bomba_id, McuEvent evento);
    
    void onStateChange(StateChangeCallback cb) { state_change_cb = cb; }
    void onBombaEvent(BombaEventCallback cb) { bomba_event_cb = cb; }
    
    // Debug
    void printState() const;
    uint32_t getUptime() const { return millis() - boot_time; }

private:
    SystemState current_state;
    SystemState previous_state;
    uint32_t boot_time;
    uint32_t last_heartbeat;
    uint32_t last_mode_check;
    
    // Estado de cada bomba
    struct BombaStatus {
        bool plc_order = false;      // PLC pide arranque
        bool feedback = false;       // Retorno aux confirma
        bool relay_closed = false;   // Relé nuestro cerrado
        uint32_t last_change = 0;    // Último cambio estado PLC/feedback
        uint32_t relay_change = 0;   // Último cambio de salida de relé
        uint8_t fault_count = 0;     // Contador fallos (sticky hasta reset)
        bool fault_latched = false;  // Evita repetir fault en el mismo episodio
    } bombas[NUM_BOMBAS];
    
    // Modo actual (detectado por hardware)
    bool modo_generador_hw = false;
    // Endurecimiento por comando remoto (solo puede pasar a true: nadie
    // declara RED por encima del pin del ATS).
    bool modo_forzado_gen = false;

    // Modo efectivo = hardware O forzado. El hardware nunca se relaja.
    bool modoEfectivo() const { return modo_generador_hw || modo_forzado_gen; }
    bool modo_mantenimiento = false;
    bool emergencia_activa = false;
    uint16_t emergencia_codigo = 0;
    uint8_t trip_attempts = 0;      // intentos de trip en la ventana
    
    // Callbacks
    StateChangeCallback state_change_cb = nullptr;
    BombaEventCallback bomba_event_cb = nullptr;
    
    // Transiciones de estado
    void transitionTo(SystemState new_state);
    SystemState computeDesiredState();
    
    // Lógica por estado
    void handleNormal();
    void handleGenerador();
    void handleEmergencia();
    void handleMantenimiento();
    
    // Utilidades
    void updateBombas();
    void checkFeedbackTimeout();
    void sendHeartbeat();
    void readModoHardware();
    uint8_t leerNivelAgua();
    
    // Anti-rebote modo
    uint8_t modo_debounce_cnt = 0;
    bool modo_estable = false;
};

#endif // STATE_MACHINE_H