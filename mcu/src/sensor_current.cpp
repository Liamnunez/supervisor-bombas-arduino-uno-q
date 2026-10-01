/**
 * @file sensor_current.cpp
 * @brief Lectura de corriente del generador (CT propio)
 */

#include "sensor_current.h"
#include <Arduino.h>

void SensorCurrent::begin() {
    pinMode(PIN_CORRIENTE_ADC, INPUT);
    // Nota: en el arranque el ADC del entorno Arduino ya está inicializado.
}

float SensorCurrent::readAmps() {
    int raw = analogRead(PIN_CORRIENTE_ADC);
    if (raw < 0) raw = 0;

    // Voltaje DC del front-end -> fracción de full-scale -> amperios
    float v = (float)raw * (CT_V_MAX / 4095.0f);
    float amps = (v / CT_V_MAX) * CT_FULL_SCALE_A;

    last_amps = amps;
    return amps;
}
