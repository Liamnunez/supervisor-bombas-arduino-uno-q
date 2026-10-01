/**
 * @file current_protector.cpp
 * @brief Implementación protección de corriente - lógica pura (sin Arduino)
 */

#include "current_protector.h"

ProtectorEvent CurrentProtector::update(uint32_t now_ms, float amps, const bool* relay_closed) {
    // --- Delta time (primer call = 0, tope 1s por seguridad) ---
    uint32_t dt = 0;
    if (have_last_ms_) {
        dt = now_ms - last_ms_;  // unsigned: correcto también con wraparound de millis()
        if (dt > 1000) dt = 1000;
    }
    have_last_ms_ = true;
    last_ms_ = now_ms;

    last_amps_ = amps;
    if (amps > peak_amps_) peak_amps_ = amps;

    // --- 1. Validez del sensor (CT) ---
    bool ct_ok = (amps >= 0.0f) && (amps <= cfg_.ct_max_amps);
    if (!ct_ok) {
        ct_invalid_t_ += dt;
        trip_t_ = 0;
        warn_t_ = 0;
        weld_t_ = 0;
        if (ct_invalid_t_ >= cfg_.ct_fault_delay_ms && !ct_latched_) {
            ct_latched_ = true;
            return ProtectorEvent::CT_FAULT;
        }
        return ProtectorEvent::NONE;
    }
    ct_invalid_t_ = 0;
    ct_latched_ = false;

    // --- 2. Sobrecarga sostenida (time-overcurrent) ---
    if (amps >= cfg_.trip_amps) {
        trip_t_ += dt;
        if (trip_t_ >= cfg_.trip_delay_ms && armed_) {
            armed_ = false;
            trip_latched_ = true;
            return ProtectorEvent::TRIP_OVERLOAD;
        }
    } else if (amps <= cfg_.reset_amps) {
        trip_t_ = 0;
        // Re-armar tras condición limpia sostenida
        if (!armed_) {
            reset_t_ += dt;
            if (reset_t_ >= cfg_.reset_delay_ms) {
                armed_ = true;
                trip_latched_ = false;
                reset_t_ = 0;
            }
        }
    } else {
        // Entre reset y trip: mantiene el acumulador (no resetea ni avanza)
        reset_t_ = 0;
    }

    // --- 3. Corriente fluyendo con relés abiertos (contactor pegado) ---
    bool any_closed = false;
    if (relay_closed != nullptr) {
        for (uint8_t i = 0; i < cfg_.num_relays; i++) {
            if (relay_closed[i]) { any_closed = true; break; }
        }
    }
    if (!any_closed && amps >= cfg_.welded_amps) {
        weld_t_ += dt;
        if (weld_t_ >= cfg_.welded_delay_ms && !weld_latched_) {
            weld_latched_ = true;
            return ProtectorEvent::CONTACTOR_WELDED;
        }
    } else {
        weld_t_ = 0;
        weld_latched_ = false;
    }

    // --- 4. Aviso de proximidad al límite ---
    if (amps >= cfg_.warn_amps && amps < cfg_.trip_amps) {
        warn_t_ += dt;
        if (warn_t_ >= cfg_.warn_delay_ms && !warn_latched_) {
            warn_latched_ = true;
            return ProtectorEvent::WARN_OVERLOAD;
        }
    } else if (amps < cfg_.warn_amps) {
        warn_t_ = 0;
        warn_latched_ = false;
    }

    return ProtectorEvent::NONE;
}

void CurrentProtector::rearm() {
    armed_ = true;
    trip_latched_ = false;
    trip_t_ = 0;
    reset_t_ = 0;
}
