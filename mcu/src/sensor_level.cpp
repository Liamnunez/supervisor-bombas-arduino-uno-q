/**
 * @file sensor_level.cpp
 * @brief Implementación sensor nivel 4-20mA
 * 
 * Circuito: Sensor 4-20mA + Resistencia 120Ω a GND
 * 4mA  -> 0.48V -> ADC ~780 (12-bit, 3.3V ref)
 * 20mA -> 2.40V -> ADC ~3900
 */

#include "sensor_level.h"
#include <Arduino.h>

SensorLevel::SensorLevel() {}

void SensorLevel::begin() {
    pinMode(PIN_NIVEL_ADC, INPUT_ANALOG);
    analogReadResolution(12);  // 0-4095
    
    // Llenar buffer inicial
    for (uint8_t i = 0; i < NUM_SAMPLES; i++) {
        samples[i] = analogRead(PIN_NIVEL_ADC);
        delay(2);
    }
    buffer_full = true;
    sample_idx = 0;
    
    update();  // Primera lectura real
}

void SensorLevel::update() {
    adc_raw = readFilteredADC();
    current_mA = adcToCurrent(adc_raw);
    nivel_porcentaje = currentToPercentage(current_mA);
    checkSensorHealth();
}

uint16_t SensorLevel::readFilteredADC() {
    // Media móvil simple
    uint32_t sum = 0;
    for (uint8_t i = 0; i < NUM_SAMPLES; i++) {
        sum += samples[i];
    }
    uint16_t avg = sum / NUM_SAMPLES;
    
    // Nueva muestra
    samples[sample_idx] = analogRead(PIN_NIVEL_ADC);
    sample_idx = (sample_idx + 1) % NUM_SAMPLES;
    if (sample_idx == 0) buffer_full = true;
    
    return avg;
}

float SensorLevel::adcToCurrent(uint16_t adc) const {
    // 3.3V / 4095 = 0.805mV per LSB
    // Voltage = adc * 3.3 / 4095
    // Current = Voltage / 120Ω * 1000 (mA)
    float voltage = adc * (3.3f / 4095.0f);
    return voltage / 0.120f;  // 120Ω = 0.120V per mA
}

uint8_t SensorLevel::currentToPercentage(float mA) const {
    if (mA <= 4.0f) return 0;
    if (mA >= 20.0f) return 100;
    
    // Lineal 4-20mA -> 0-100%
    float pct = (mA - 4.0f) * 100.0f / 16.0f;
    return constrain(static_cast<uint8_t>(pct + 0.5f), 0, 100);
}

void SensorLevel::checkSensorHealth() {
    // Sensor OK si corriente entre 3.5 y 21mA (margen)
    sensor_ok = (current_mA >= 3.5f && current_mA <= 21.0f);
    
    // También validar ADC en rango esperado
    if (adc_raw < 600 || adc_raw > 4000) {
        sensor_ok = false;
    }
}