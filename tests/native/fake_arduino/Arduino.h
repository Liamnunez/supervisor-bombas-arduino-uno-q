/**
 * @file Arduino.h
 * @brief Shim: hace que <Arduino.h> resuelva al doble de test.
 *
 * Compilando con `-I tests/native/fake_arduino` y `-include Arduino.h`
 * (igual que hace el job firmware-syntax de CI), cualquier include de
 * Arduino.h que haga el firmware trae aquí el doble de test en lugar del
 * stub de CI, que no permite ejecutar nada.
 */
#ifndef FAKE_ARDUINO_SHIM_H
#define FAKE_ARDUINO_SHIM_H

#include "../arduino_fake.h"

#endif  // FAKE_ARDUINO_SHIM_H