/**
 * @file trip_policy.cpp
 * @brief Implementación de la política de reintentos del trip (SIF-04)
 */

#include "trip_policy.h"

namespace {

// Resta con signo: correcta aunque millis() dé la vuelta (~49 días).
// true si `now` ya alcanzó o pasó `deadline`.
inline bool alcanzo(uint32_t now, uint32_t deadline) {
    return static_cast<int32_t>(now - deadline) >= 0;
}

// Antigüedad de `ts` en ms, sin romper con el wraparound de millis().
inline uint32_t antiguedad(uint32_t now, uint32_t ts) {
    return now - ts;
}

}  // namespace

uint8_t TripPolicy::maxAttempts() const {
    uint8_t n = cfg_.max_attempts;
    if (n == 0) n = 1;                                  // al menos 1 intento
    if (n > TRIP_MAX_SLOTS) n = TRIP_MAX_SLOTS;         // no desbordar el array
    return n;
}

void TripPolicy::prune(uint32_t now_ms) {
    // Descarta intentos más antiguos que la ventana (compacta in-place).
    uint8_t keep = 0;
    for (uint8_t i = 0; i < attempts_; i++) {
        if (antiguedad(now_ms, attempt_ms_[i]) <= cfg_.window_ms) {
            attempt_ms_[keep++] = attempt_ms_[i];
        }
    }
    attempts_ = keep;
}

bool TripPolicy::cooldownVencido(uint32_t now_ms) const {
    return alcanzo(now_ms, cooldown_until_);
}

TripDecision TripPolicy::update(uint32_t now_ms, bool trip_now, float amps) {
    prune(now_ms);

    // Ya latcheado por intentos agotados: no se cuentan más intentos ni
    // se re-arma. Solo operatorReset() abre esta puerta.
    if (latched_) return TripDecision::NONE;

    // --- Trip en este ciclo: contar el intento ---
    if (trip_now) {
        if (attempts_ < TRIP_MAX_SLOTS) {
            attempt_ms_[attempts_++] = now_ms;
        }
        if (attempts_ >= maxAttempts()) {
            latched_ = true;
            // Edge: avisar UNA vez. Después devuelve NONE pero sigue
            // latcheado, así el llamador no re-spam de alertas a 100 Hz.
            if (!hold_reported_) {
                hold_reported_ = true;
                return TripDecision::HOLD;
            }
            return TripDecision::NONE;
        }
        // Still room to retry: open the cooldown for the next re-arm.
        cooldown_until_ = now_ms + cfg_.cooldown_ms;
        return TripDecision::NONE;
    }

    // --- Sin trip: ¿toca re-armar automáticamente? ---
    if (attempts_ == 0) return TripDecision::NONE;
    if (!cooldownVencido(now_ms)) return TripDecision::NONE;
    // No re-armar mientras la bomba siga consumiendo (sería re-armar en
    // corto): la corriente tiene que estar limpia de verdad.
    if (amps > cfg_.recover_amps) {
        // Re-armar igual perdería la condición: extendemos la espera.
        cooldown_until_ = now_ms + cfg_.cooldown_ms;
        return TripDecision::NONE;
    }

    // RECOVER consume un turno de enfriamiento: evita repetir la decisión
    // en cada ciclo del loop mientras la corriente siga limpia.
    cooldown_until_ = now_ms + cfg_.cooldown_ms;
    return TripDecision::RECOVER;
}

void TripPolicy::operatorReset() {
    attempts_ = 0;
    latched_ = false;
    hold_reported_ = false;
    cooldown_until_ = 0;
}

uint32_t TripPolicy::retryInMs(uint32_t now_ms) const {
    if (latched_ || attempts_ == 0) return 0;
    if (cooldownVencido(now_ms)) return 0;
    return cooldown_until_ - now_ms;
}

uint8_t TripPolicy::remaining() const {
    uint8_t max = maxAttempts();
    if (attempts_ >= max) return 0;
    return static_cast<uint8_t>(max - attempts_);
}
