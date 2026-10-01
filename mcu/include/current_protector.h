/**
 * @file current_protector.h
 * @brief Protección de corriente del generador - lógica PURA (sin Arduino)
 *
 * Vigila la corriente total del generador con un CT PROPIO (no se modifica
 * el CT ni la configuración del DSE 7320) y actúa ANTES de que el DSE
 * corte por sobrecarga:
 *
 *   - AVISO:      >= CORRIENTE_AVISO_A durante 1s
 *   - TRIP:       >= CORRIENTE_TRIP_A durante 3s (time-overcurrent,
 *                  tolera el inrush de arranque de motor ~2s)
 *   - PEGADO:     corriente fluyendo con TODOS los relés abiertos
 *                 -> contactor soldado o carga externa
 *   - CT_FAULT:   lectura fuera de rango (sensor desconectado/calibración)
 *
 * Diseñado como lógica pura (solo <stdint.h>) para poder testearse
 * nativamente con g++ en tests/native/ - sin hardware.
 */

#ifndef CURRENT_PROTECTOR_H
#define CURRENT_PROTECTOR_H

#include <stdint.h>

/** Evento emitido por el protector (uno por update como máximo) */
enum class ProtectorEvent : uint8_t {
    NONE = 0,
    WARN_OVERLOAD,      // Cerca del límite del generador
    TRIP_OVERLOAD,      // Sobrecarga sostenida -> EMERGENCIA
    CONTACTOR_WELDED,    // Corriente con relés abiertos
    CT_FAULT             // Sensor de corriente inválido
};

/** Configuración del protector (valores por defecto = preset generador 45A) */
struct ProtectorConfig {
    float warn_amps = 40.0f;             // Umbral de aviso
    float trip_amps = 42.0f;             // Umbral de trip (antes del corte DSE)
    float reset_amps = 37.0f;            // Histeresis de re-armado
    float welded_amps = 2.0f;            // Corriente mínima con relés abiertos
    float ct_max_amps = 120.0f;          // Fuera de rango -> CT fault
    uint32_t trip_delay_ms = 3000;       // Sostenida para trip (> inrush)
    uint32_t warn_delay_ms = 1000;       // Sostenida para aviso
    uint32_t reset_delay_ms = 2000;      // Condición limpia para re-armar
    uint32_t welded_delay_ms = 1000;     // Sostenida para detectar pegado
    uint32_t ct_fault_delay_ms = 500;    // Lectura inválida sostenida
    uint8_t num_relays = 3;              // Número de bombas/relés
};

class CurrentProtector {
public:
    CurrentProtector() {}
    explicit CurrentProtector(const ProtectorConfig& cfg) : cfg_(cfg) {}

    /**
     * Avanza la máquina de detección.
     * @param now_ms        millis() del sistema
     * @param amps          corriente RMS medida (negativa o > ct_max = inválida)
     * @param relay_closed  estado real de cada relé (num_relays elementos)
     * @return              evento detectado en este ciclo (NONE si no hay)
     */
    ProtectorEvent update(uint32_t now_ms, float amps, const bool* relay_closed);

    /** Re-armar el trip tras resolver la emergencia (reset manual) */
    void rearm();

    bool tripped() const { return trip_latched_; }
    float lastAmps() const { return last_amps_; }
    float peakAmps() const { return peak_amps_; }
    void resetPeak() { peak_amps_ = 0.0f; }

private:
    ProtectorConfig cfg_;

    float last_amps_ = 0.0f;
    float peak_amps_ = 0.0f;

    // Temporizadores (acumulados en ms)
    uint32_t trip_t_ = 0;
    uint32_t warn_t_ = 0;
    uint32_t weld_t_ = 0;
    uint32_t ct_invalid_t_ = 0;
    uint32_t reset_t_ = 0;

    // Latches: emiten un solo evento por excursión
    bool trip_latched_ = false;
    bool warn_latched_ = false;
    bool weld_latched_ = false;
    bool ct_latched_ = false;
    bool armed_ = true;       // Trip re-armable

    // Delta time
    bool have_last_ms_ = false;
    uint32_t last_ms_ = 0;
};

#endif // CURRENT_PROTECTOR_H
