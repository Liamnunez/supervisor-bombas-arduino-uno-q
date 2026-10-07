/**
 * @file pnoz_heartbeat.h
 * @brief Generador de latido para el relé de seguridad Pilz PNOZ s4
 *
 * Conecta el firmware con la barrera de seguridad EXTERNA. El PNOZ s4
 * configurado en modo watchdog (entradas P1/P2) abre sus salidas de
 * seguridad 13-14 y 23-24 si el pulso se interrumpe. Con los relés de las
 * bombas alimentados a través de esas salidas, un firmware colgado deja
 * las bombas paradas SIN depender de ninguna lógica del software.
 *
 * Esta clase solo decide el nivel del pin; el escritura la hace main.cpp.
 * El pin queda en LOW en reposo (estado seguro: "no hay pulso"), por lo
 * que si el MCU se cuelga con el pin en LOW el PNOZ abre los relés.
 *
 * Periodo por defecto 100 ms con pulso de 10 ms (un ciclo del loop de
 * 100 Hz). El tiempo exacto debe ajustarse al manual del PNOZ s4 y
 * verificarse en banco antes de instalar - ver docs/hardware_spec.md.
 *
 * Lógica pura (solo <stdint.h>) para testearse nativamente con g++ en
 * tests/native/ - sin hardware.
 */

#ifndef PNOZ_HEARTBEAT_H
#define PNOZ_HEARTBEAT_H

#include <stdint.h>

class PnozHeartbeat {
public:
    /** @param period_ms periodo entre inicio de pulso y el siguiente */
    explicit PnozHeartbeat(uint32_t period_ms = 100) : period_ms_(period_ms) {}

    /**
     * Avanza el generador de pulso. LLAMAR UNA VEZ POR CICLO del loop
     * de 100 Hz (10 ms) para que el ancho de pulso sea determinista.
     *
     * @param now_ms millis() del sistema
     * @return true si el pin debe estar HIGH en este ciclo
     */
    bool update(uint32_t now_ms);

    /** Vuelve al estado seguro (LOW, sin pulso) y reinicia el contador */
    void reset();

    bool isHigh() const { return high_; }
    uint32_t pulses() const { return pulses_; }
    uint32_t periodMs() const { return period_ms_; }

private:
    uint32_t period_ms_;
    uint32_t last_pulse_ms_ = 0;
    uint32_t pulses_ = 0;
    bool high_ = false;     // reposo = LOW = sin pulso (fail-safe)
    bool started_ = false;
};

#endif // PNOZ_HEARTBEAT_H
