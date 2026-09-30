/**
 * @file mode_detect.cpp
 * @brief Implementación detección modo RED/GENERADOR
 * 
 * Contacto seco del inversor:
 * - Cerrado (pull-up -> HIGH) = RED
 * - Abierto (pull-down -> LOW) = GENERADOR
 */

#include "mode_detect.h"
#include <Arduino.h>

ModeDetect::ModeDetect() {}

void ModeDetect::begin() {
    pinMode(PIN_MODO_GEN, INPUT_PULLUP);
    
    // Lecturas iniciales para estabilizar
    for (uint8_t i = 0; i < DEBOUNCE_READINGS; i++) {
        ultima_lectura = digitalRead(PIN_MODO_GEN);
        delay(5);
    }
    modo_generador = !ultima_lectura;  // LOW = GENERADOR
    estable = true;
    contador_estable = DEBOUNCE_READINGS;
}

void ModeDetect::update() {
    if (forzado_activo) {
        modo_generador = modo_forzado;
        estable = true;
        return;
    }
    
    bool lectura = digitalRead(PIN_MODO_GEN);  // HIGH=RED, LOW=GEN
    
    if (lectura == ultima_lectura) {
        if (contador_estable < DEBOUNCE_READINGS) {
            contador_estable++;
        }
    } else {
        contador_estable = 0;
        ultima_lectura = lectura;
    }
    
    if (contador_estable >= DEBOUNCE_READINGS) {
        estable = true;
        modo_generador = !lectura;  // LOW = GENERADOR
    }
}

void ModeDetect::setForzado(bool generador, bool forzar) {
    forzado_activo = forzar;
    if (forzar) {
        modo_forzado = generador;
    }
}

void ModeDetect::clearForzado() {
    forzado_activo = false;
}

bool ModeDetect::leerPin() const {
    return digitalRead(PIN_MODO_GEN);
}