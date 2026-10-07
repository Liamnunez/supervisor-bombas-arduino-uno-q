/**
 * @file trip_policy.h
 * @brief Política de reintentos del trip por sobrecorriente (SIF-04)
 *
 * El protector de corriente detecta la sobrecarga y abre todos los relés.
 * La decisión de **cuándo volver a intentarlo** es política de operación,
 * no detección, por eso vive aparte.
 *
 * Motivación (acordada en Fase 5): el generador no soporta 2 bombas, así
 * que la única forma de bombear en generador es con 1 bomba. Si el trip
 * deja el sistema parado hasta que llegue un operador, en una tormenta
 * nocturna el pozo se desborda. El objetivo es que el sistema se
 * recupere solo sin abandonar la protección del generador.
 *
 * Regla:
 *   - Cada trip cuenta un intento.
 *   - Con la corriente ya limpia, tras un enfriamiento se re-arma solo.
 *   - Al agotar los intentos dentro de la ventana, NO re-arma más: queda
 *     latcheado y espera a una persona. Si falla 3 veces el problema ya
 *     no es transitorio y no tiene sentido insistir.
 *
 * Lógica pura (solo <stdint.h>) para testearse nativamente con g++ en
 * tests/native/ - sin hardware.
 */

#ifndef TRIP_POLICY_H
#define TRIP_POLICY_H

#include <stdint.h>

/** Máximo de intentos guardados en el histórico circular */
#define TRIP_MAX_SLOTS 8

/** Decisión de la política para este ciclo */
enum class TripDecision : uint8_t {
    NONE = 0,     // nada que hacer (la emergencia ya la puso el protector)
    RECOVER,      // recuperar solo: limpiar emergencia y re-armar
    HOLD          // intentos agotados: mantener emergencia, esperar operador
};

struct TripPolicyConfig {
    uint8_t max_attempts = 3;       // intentos antes de exigir operador
    uint32_t window_ms = 900000;    // 15 min - ventana de conteo
    uint32_t cooldown_ms = 60000;   // 60 s de espera entre reintentos
    float recover_amps = 5.0f;      // corriente máxima para re-intentar
};

class TripPolicy {
public:
    TripPolicy() {}
    explicit TripPolicy(const TripPolicyConfig& cfg) : cfg_(cfg) {}

    /**
     * Avanza la política. Llamar una vez por ciclo del loop.
     *
     * @param now_ms   millis() del sistema
     * @param trip_now el protector emitió TRIP_OVERLOAD en este ciclo
     * @param amps     corriente RMS medida ahora
     * @return         RECOVER solo en el ciclo en que toca re-armar (edge)
     */
    TripDecision update(uint32_t now_ms, bool trip_now, float amps);

    /** Reset del operador: limpia intentos y latch */
    void operatorReset();

    /** Intentos dentro de la ventana vigente */
    uint8_t attempts() const { return attempts_; }

    /** Latcheado por intentos agotados (solo lo abre el operador) */
    bool latched() const { return latched_; }

    /** Cuánto falta para el próximo re-armado (0 si ya se puede) */
    uint32_t retryInMs(uint32_t now_ms) const;

    /** Intentos que quedan antes de exigir operador */
    uint8_t remaining() const;

    const TripPolicyConfig& config() const { return cfg_; }

private:
    TripPolicyConfig cfg_;
    uint32_t attempt_ms_[TRIP_MAX_SLOTS] = {0};
    uint8_t attempts_ = 0;
    uint32_t cooldown_until_ = 0;
    bool latched_ = false;
    bool hold_reported_ = false;

    void prune(uint32_t now_ms);
    uint8_t maxAttempts() const;
    bool cooldownVencido(uint32_t now_ms) const;
};

#endif // TRIP_POLICY_H
