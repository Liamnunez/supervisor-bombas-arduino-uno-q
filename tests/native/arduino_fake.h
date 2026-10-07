/**
 * @file arduino_fake.h
 * @brief Doble de test para Arduino.h - pines, millis() y puertos serie
 *        controlados desde el test.
 *
 * POR QUÉ ESTO EXISTE
 * --------------------
 * `mcu/src/state_machine.cpp` es la única pieza que decide si una bomba
 * puede arrancar, y depende de Arduino.h (digitalRead / digitalWrite /
 * millis). El stub que construye CI (.github/workflows/ci.yml) solo sirve
 * para `-fsyntax-only`: sus pines siempre valen 0 y su millis() siempre
 * vale 0, así que con él no se puede ejecutar ni un solo ciclo de la
 * máquina de estados.
 *
 * Este doble mantiene estado real y observable:
 *   - el nivel y el modo de cada pin se fijan desde el test,
 *   - millis() avanza solo cuando el test lo mueve (determinismo total),
 *   - Serial/Serial1 capturan lo que el firmware escribe y permiten
 *     inyectar bytes de RX.
 *
 * Se usa como si fuera <Arduino.h>: se añade -I tests/native/fake_arduino
 * al compilador y ese directorio contiene un Arduino.h que sólo incluye
 * este fichero.
 *
 * SIN DEPENDENCIAS: sólo <stdint.h>/<stddef.h>/<vector>.
 */

#ifndef FAKE_ARDUINO_H
#define FAKE_ARDUINO_H

#include <stdint.h>
#include <stddef.h>
#include <vector>

// ============================================================
// Niveles y modos (valores reales del core de Arduino)
// ============================================================
#define HIGH 1
#define LOW  0
#define INPUT        0
#define OUTPUT       1
#define INPUT_PULLUP   2
#define INPUT_PULLDOWN 3
#define INPUT_ANALOG   4

// --- Pines del STM32U585 (mismos valores que el stub de CI) ---
#define PA0 0
#define PA1 1
#define PA2 2
#define PA3 3
#define PA4 4
#define PA5 5
#define PA6 6
#define PA7 7
#define PA8 8
#define PA9 9
#define PA10 10
#define PA11 11
#define PA12 12
#define PA13 13
#define PA14 14
#define PA15 15

#define PB0 16
#define PB1 17
#define PB2 18
#define PB3 19
#define PB4 20
#define PB5 21
#define PB6 22
#define PB7 23
#define PB8 24
#define PB9 25
#define PB10 26
#define PB11 27
#define PB12 28
#define PB13 29
#define PB14 30
#define PB15 31

#define PC0 32
#define PC1 33
#define PC2 34
#define PC3 35
#define PC4 36
#define PC5 37
#define PC6 38
#define PC7 39
#define PC8 40
#define PC9 41
#define PC10 42
#define PC11 43
#define PC12 44
#define PC13 45

#define constrain(x, lo, hi) ((x) < (lo) ? (lo) : ((x) > (hi) ? (hi) : (x)))

namespace fake {

// Número de pines modelados (PA0..PA15 + PB0..PB15 + PC0..PC13).
constexpr int NUM_PINES = 128;

struct Pin {
    int      level = LOW;   // lo que leería digitalRead()
    int      mode  = INPUT; // lo que se le puso con pinMode()
    int      escrituras = 0;// cuántas veces se escribió con digitalWrite()
    int      ultima = LOW;  // último nivel escrito
};

struct State {
    Pin  pines[NUM_PINES];
    uint32_t reloj = 0;                 // valor que devuelve millis()
    std::vector<uint8_t> tx;            // último byte escrito en Serial1
};

// El estado es global y compartido entre el test y el .cpp del firmware
// (variables inline de C++17: una sola definición, sin ODR).
inline State& st() {
    static State s;
    return s;
}

/** Devuelve el reloj simulado (lo que devolvería millis()). */
inline uint32_t& reloj() { return st().reloj; }

/** Fija el reloj. Sin esto los tests dependerían del orden de ejecución. */
inline void setMillis(uint32_t ms) { st().reloj = ms; }

/** Avanza el reloj, como si Between-loops pasara tiempo. */
inline void advance(uint32_t ms) { st().reloj += ms; }

/** Fija el nivel de un pin (lo que vería digitalRead()). */
inline void setPin(int pin, int level) {
    if (pin >= 0 && pin < NUM_PINES) st().pines[pin].level = level;
}

/** Nivel actual de un pin. */
inline int getPin(int pin) {
    return (pin >= 0 && pin < NUM_PINES) ? st().pines[pin].level : LOW;
}

/** Fija el modo de un pin. */
inline void setMode(int pin, int mode) {
    if (pin >= 0 && pin < NUM_PINES) st().pines[pin].mode = mode;
}

/** Modo configurado en un pin (INPUT / INPUT_PULLUP / OUTPUT...). */
inline int getMode(int pin) {
    return (pin >= 0 && pin < NUM_PINES) ? st().pines[pin].mode : INPUT;
}

/** Cuántas veces se escribió el pin (para detectar colgados). */
inline int escrituras(int pin) {
    return (pin >= 0 && pin < NUM_PINES) ? st().pines[pin].escrituras : 0;
}

/** Deja el pin en el nivel que modela un cable cortado (flotante). */
inline void cortarCable(int pin) {
    // Con pull-up  -> el pin flotante cae a HIGH (que el firmware lee RED)
    // Con pull-down -> el pin flotante cae a LOW  (que el firmware lee sin señal)
    switch (getMode(pin)) {
        case INPUT_PULLUP:   setPin(pin, HIGH); break;
        case INPUT_PULLDOWN: setPin(pin, LOW);  break;
        default:             setPin(pin, LOW);  break;
    }
}

/** Deja todos los pines como INPUT (flotantes) - estado tras el reset. */
inline void pinesFlotantes() {
    for (int i = 0; i < NUM_PINES; i++) {
        st().pines[i].level = LOW;
        st().pines[i].mode  = INPUT;
        st().pines[i].escrituras = 0;
        st().pines[i].ultima = LOW;
    }
}

/** Reinicia el doble por completo entre tests. */
inline void reset() {
    pinesFlotantes();
    st().reloj = 0;
    st().tx.clear();
}

/** Serial de test: captura TX e inyecta RX. */
struct FakeSerial {
    std::vector<uint8_t> cola;  // pendiente de leer (RX)
    std::vector<uint8_t> emitted;  // todo lo escrito (TX)
    size_t lectura_ = 0;        // índice del primer byte no leído

    void begin(unsigned long) {}
    explicit operator bool() const { return true; }

    size_t write(const uint8_t* data, size_t n) {
        for (size_t i = 0; i < n; i++) {
            emitted.push_back(data[i]);
            st().tx.push_back(data[i]);
        }
        return n;
    }

    void flush() {}

    /**
     * `lectura_` es el índice del primer byte pendiente. NO se usa
     * cola.erase(begin()) porque es O(n) POR BYTE: el firmware lee byte a
     * byte y, con ruido de 1000 bytes, eso son ~500 000 movements de
     * memoria y el test se cuelga. Con un índice, leer es O(1) y el
     * contenido y el orden observados por el firmware son idénticos.
     */
    size_t pendientes() const { return cola.size() - lectura_; }

    void drenar_rapido() {
        lectura_ = cola.size();
    }

    int available() { return static_cast<int>(pendientes()); }

    int read() {
        if (lectura_ >= cola.size()) return -1;
        return cola[lectura_++];
    }

    // El firmware imprime por Serial en printState() y en el debug.
    template <typename T> size_t print(const T&) { return 0; }
    template <typename T> size_t print(const T&, int) { return 0; }
    template <typename T> size_t println(const T&) { return 0; }
    template <typename T> size_t println(const T&, int) { return 0; }
    size_t println() { return 0; }

    /** Inyecta bytes como si los hubiera recibido del otro lado. */
    void feed(const uint8_t* data, size_t n) {
        for (size_t i = 0; i < n; i++) cola.push_back(data[i]);
    }

    /** Descarta todo lo pendiente (RX limpio). */
    void limpiar_rx() {
        cola.clear();
        lectura_ = 0;
    }

    /** Cuántos bytes siguen sin leer por el firmware. */
    size_t rx_pendientes() const { return pendientes(); }
};

inline FakeSerial& serial0() { static FakeSerial s; return s; }
inline FakeSerial& serial1() { static FakeSerial s; return s; }

}  // namespace fake

// --- Globales que espera el firmware ---
inline fake::FakeSerial& Serial  = fake::serial0();
inline fake::FakeSerial& Serial1 = fake::serial1();

// --- API del core ---
inline void pinMode(int pin, int mode) { fake::setMode(pin, mode); }
inline void digitalWrite(int pin, int level) {
    if (pin >= 0 && pin < fake::NUM_PINES) {
        fake::st().pines[pin].escrituras++;
        fake::st().pines[pin].ultima = level;
        fake::st().pines[pin].level = level;
    }
}
inline int digitalRead(int pin) { return fake::getPin(pin); }
inline int analogRead(int) { return 0; }
inline void analogReadResolution(int) {}
inline uint32_t millis() { return fake::reloj(); }
/** delay() hace avanzar el reloj: el test sigue siendo determinista. */
inline void delay(uint32_t ms) { fake::advance(ms); }
inline long map(long x, long in_min, long in_max, long out_min, long out_max) {
    return out_min + (x - in_min) * (out_max - out_min) / (in_max - in_min);
}

// --- IWDG (lo usa setup() de main.cpp) ---
struct FakeIWDG { volatile uint32_t KR, PR, RLR; };
inline FakeIWDG fake_iwdg_instance{};
#define IWDG (&fake_iwdg_instance)

#endif  // FAKE_ARDUINO_H