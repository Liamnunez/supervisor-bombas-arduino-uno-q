/**
 * @file sensor_current.cpp
 * @brief Lectura de corriente del generador (CT propio)
 */

#include "sensor_current.h"
#include <Arduino.h>

void SensorCurrent::begin() {
    // INPUT_ANALOG, no INPUT. INPUT deja el pin como entrada digital y
    // desconecta el periférico ADC, así que analogRead() no mide nada.
    //
    // Este pin es el único sensor de SIF-04 (la barrera software contra 2
    // bombas en el generador). Con INPUT leería ~0 A, el protector nunca
    // vería sobrecarga y PT-05 no podría ejecutarse. sensor_level.cpp usa
    // INPUT_ANALOG para PA4: mismo tipo de señal, tratamiento distinto.
    pinMode(PIN_CORRIENTE_ADC, INPUT_ANALOG);
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
