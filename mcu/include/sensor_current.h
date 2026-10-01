/**
 * @file sensor_current.h
 * @brief Lectura corriente generador - CT PROPIO (no modifica el CT del DSE 7320)
 *
 * Hardware: pinza/CT en la salida del generador -> front-end analógico
 * (rectificación + filtro) -> voltaje DC 0..CT_V_MAX proporcional a la
 * corriente RMS. NO se toca ningún sensor ni parámetro del DSE 7320.
 *
 * IMPORTANTE: calibrar CT_FULL_SCALE_A con pinza amperométrica en la
 * instalación antes de confiar en los valores absolutos.
 */

#ifndef SENSOR_CURRENT_H
#define SENSOR_CURRENT_H

#include "config.h"

class SensorCurrent {
public:
    void begin();

    /** Corriente RMS estimada en amperios */
    float readAmps();

    /** Última lectura (sin volver a tocar el ADC) */
    float lastAmps() const { return last_amps; }

private:
    float last_amps = 0.0f;
};

#endif // SENSOR_CURRENT_H
