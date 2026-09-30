/**
 * @file feedback.cpp
 * @brief Implementación lectura retornos auxiliares contactores
 * 
 * Retornos auxiliares NO (Normalmente Abierto):
 * - Contacto cerrado (HIGH) = Contactor energizado = Bomba corriendo
 * - Contacto abierto (LOW) = Contactor en reposo = Bomba parada
 */

#include "feedback.h"
#include <Arduino.h>

Feedback::Feedback() {
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        feedback_states[i] = false;
        last_raw[i] = false;
        last_change[i] = 0;
        debounce_cnt[i] = 0;
    }
}

void Feedback::begin() {
    pinMode(PIN_FEEDBACK_B1, INPUT_PULLDOWN);
    pinMode(PIN_FEEDBACK_B2, INPUT_PULLDOWN);
    pinMode(PIN_FEEDBACK_B3, INPUT_PULLDOWN);
    
    // Lectura inicial
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bool raw = digitalRead(pinForBomba(i));
        last_raw[i] = raw;
        feedback_states[i] = raw;
    }
}

void Feedback::update() {
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bool raw = digitalRead(pinForBomba(i));
        
        // Anti-rebote
        if (raw == last_raw[i]) {
            if (debounce_cnt[i] < 255) debounce_cnt[i]++;
        } else {
            debounce_cnt[i] = 0;
            last_raw[i] = raw;
        }
        
        // Confirmar cambio tras debounce
        if (debounce_cnt[i] >= (DEBOUNCE_MS / 10)) {  // @100Hz loop
            if (raw != feedback_states[i]) {
                feedback_states[i] = raw;
                last_change[i] = now;
                
                if (change_cb) {
                    change_cb(i, raw);
                }
            }
        }
    }
}

bool Feedback::getFeedback(uint8_t bomba_id) const {
    if (bomba_id >= NUM_BOMBAS) return false;
    return feedback_states[bomba_id];
}

bool Feedback::getFeedbackRaw(uint8_t bomba_id) const {
    if (bomba_id >= NUM_BOMBAS) return false;
    return last_raw[bomba_id];
}

uint8_t Feedback::pinForBomba(uint8_t id) const {
    switch (id) {
        case 0: return PIN_FEEDBACK_B1;
        case 1: return PIN_FEEDBACK_B2;
        case 2: return PIN_FEEDBACK_B3;
        default: return 0xFF;
    }
}