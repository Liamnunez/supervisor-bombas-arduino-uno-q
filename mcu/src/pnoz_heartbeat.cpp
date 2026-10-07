/**
 * @file pnoz_heartbeat.cpp
 * @brief Implementación del generador de latido hacia el PNOZ s4
 */

#include "pnoz_heartbeat.h"

bool PnozHeartbeat::update(uint32_t now_ms) {
    // El pulso dura exactamente un ciclo del loop (10 ms a 100 Hz).
    // Primero se baja el pin que estaba en HIGH en el ciclo anterior.
    if (high_) {
        high_ = false;
        return false;
    }

    // Primer ciclo tras arrancar: pulso inmediato para que el PNOZ
    // registre señal desde el principio.
    if (!started_) {
        started_ = true;
        last_pulse_ms_ = now_ms;
        high_ = true;
        pulses_++;
        return true;
    }

    // Resta sin signo: correcta aunque millis() dé la vuelta (~49 días).
    if (now_ms - last_pulse_ms_ >= period_ms_) {
        last_pulse_ms_ = now_ms;
        high_ = true;
        pulses_++;
        return true;
    }

    return false;
}

void PnozHeartbeat::reset() {
    high_ = false;
    started_ = false;
    last_pulse_ms_ = 0;
    pulses_ = 0;
}
