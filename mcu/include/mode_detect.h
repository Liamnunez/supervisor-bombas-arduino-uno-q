/**
 * @file mode_detect.h
 * @brief Detección modo RED/GENERADOR via contacto seco inversor
 */

#ifndef MODE_DETECT_H
#define MODE_DETECT_H

#include "config.h"

class ModeDetect {
public:
    ModeDetect();
    
    void begin();
    void update();
    
    bool isGenerador() const { return modo_generador; }
    bool isRed() const { return !modo_generador; }
    bool isStable() const { return estable; }
    
    // Forzar modo (desde Linux side o botón local)
    void setForzado(bool generador, bool forzar = true);
    void clearForzado();

private:
    bool modo_generador = false;   // true = GENERADOR, false = RED
    bool estable = false;
    bool forzado_activo = false;
    bool modo_forzado = false;
    
    // Anti-rebote
    static constexpr uint8_t DEBOUNCE_READINGS = 10;  // 100ms @ 100Hz
    uint8_t contador_estable = 0;
    bool ultima_lectura = true;  // Pull-up: HIGH = RED
    
    bool leerPin() const;
};

#endif // MODE_DETECT_H