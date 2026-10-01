/**
 * @file feedback.h
 * @brief Lectura retornos auxiliares de contactores (estado real bombas)
 */

#ifndef FEEDBACK_H
#define FEEDBACK_H

#include "config.h"

class Feedback {
public:
    Feedback();
    
    void begin();
    void update();
    
    bool getFeedback(uint8_t bomba_id) const;
    bool getFeedbackRaw(uint8_t bomba_id) const;
    
    // Callbacks para notificar cambios
    using FeedbackCallback = void (*)(uint8_t bomba_id, bool activo);
    void onChange(FeedbackCallback cb) { change_cb = cb; }

private:
    bool feedback_states[NUM_BOMBAS];
    bool last_raw[NUM_BOMBAS];
    uint32_t last_change[NUM_BOMBAS];
    
    // Anti-rebote: usar DEBOUNCE_MS de config.h (parámetro central).
    // Nota: no declarar aquí una constante con el mismo nombre -
    // colisiona con el macro de config.h y rompe la compilación.
    uint8_t debounce_cnt[NUM_BOMBAS];
    
    FeedbackCallback change_cb = nullptr;
    
    uint8_t pinForBomba(uint8_t id) const;
};

#endif // FEEDBACK_H