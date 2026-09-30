/**
 * @file sensor_level.h
 * @brief Lectura sensor nivel agua 4-20mA via ADC
 */

#ifndef SENSOR_LEVEL_H
#define SENSOR_LEVEL_H

#include "config.h"

class SensorLevel {
public:
    SensorLevel();
    
    void begin();
    void update();
    
    uint8_t getNivelPorcentaje() const { return nivel_porcentaje; }
    uint16_t getRawADC() const { return adc_raw; }
    float getCurrent_mA() const { return current_mA; }
    
    bool isValid() const { return sensor_ok; }
    bool isCriticoBajo() const { return nivel_porcentaje <= NIVEL_CRITICO_BAJO; }
    bool isCriticoAlto() const { return nivel_porcentaje >= NIVEL_CRITICO_ALTO; }

private:
    uint16_t adc_raw = 0;
    float current_mA = 0.0f;
    uint8_t nivel_porcentaje = 0;
    bool sensor_ok = false;
    
    // Filtrado
    static constexpr uint8_t NUM_SAMPLES = 16;
    uint16_t samples[NUM_SAMPLES];
    uint8_t sample_idx = 0;
    bool buffer_full = false;
    
    uint16_t readFilteredADC();
    float adcToCurrent(uint16_t adc) const;
    uint8_t currentToPercentage(float mA) const;
    void checkSensorHealth();
};

#endif // SENSOR_LEVEL_H