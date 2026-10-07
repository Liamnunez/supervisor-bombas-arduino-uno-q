/**
 * @file test_pnoz_heartbeat.cpp
 * @brief Tests nativos (g++) de la lógica REAL del latido PNOZ s4
 *
 * Compilar y ejecutar:  make tests-native
 * No requiere hardware ni PlatformIO - prueba el código exacto que corre en el MCU.
 */

#include "pnoz_heartbeat.h"

#include <cstdio>

static int checks = 0;
static int failures = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

// --- Simulador: avanza el tiempo como el loop de 100Hz del firmware ---
// dt_ms = 10 emula el LOOP_INTERVAL_MS de main.cpp
struct Sim {
    PnozHeartbeat hb;
    uint32_t t = 0;

    explicit Sim(uint32_t period_ms = 100) : hb(period_ms) {}

    bool step(uint32_t dt_ms = 10) {
        t += dt_ms;
        return hb.update(t);
    }
};

static void test_estado_seguro_inicial() {
    // Un MCU que acaba de arrancar y aún no emite nada debe verse como
    // "sin pulso": el PNOZ mantiene los relés abiertos.
    PnozHeartbeat hb;
    CHECK(!hb.isHigh());
    CHECK(hb.pulses() == 0);
}

static void test_primer_pulso_inmediato() {
    Sim s;
    CHECK(s.step() == true);      // primer ciclo -> pulso
    CHECK(s.hb.isHigh());
    CHECK(s.hb.pulses() == 1);
}

static void test_ancho_de_pulso_un_ciclo() {
    Sim s;
    s.step();
    CHECK(s.hb.isHigh());
    // El ciclo siguiente DEBE bajar el pin: ancho = 1 ciclo = 10 ms
    CHECK(s.step() == false);
    CHECK(!s.hb.isHigh());
    CHECK(s.step() == false);
    CHECK(s.step() == false);
    CHECK(s.hb.pulses() == 1);    // no hay pulsos extra
}

static void test_no_pulsa_antes_del_periodo() {
    Sim s(100);
    s.step();                     // t=10, pulso #1
    for (int i = 0; i < 9; i++) s.step();   // t=20..100, todavía dentro
    // t=110 - han pasado 100 ms desde el pulso -> ahora sí
    CHECK(s.step() == true);
    CHECK(s.hb.pulses() == 2);
}

static void test_cadencia_en_un_segundo() {
    // Periodo 100ms en 1 s -> ~10 pulsos (10 Hz) + el inicial.
    Sim s(100);
    for (uint32_t i = 0; i < 100; i++) s.step();   // 1 s a 100 Hz
    uint32_t p = s.hb.pulses();
    CHECK(p >= 9 && p <= 11);
    CHECK(s.hb.periodMs() == 100);
}

static void test_reset_vuelve_a_seguro() {
    Sim s;
    s.step();
    CHECK(s.hb.isHigh());
    s.hb.reset();
    CHECK(!s.hb.isHigh());
    CHECK(s.hb.pulses() == 0);
    CHECK(s.step() == true);      // y vuelve a pulsar
    CHECK(s.hb.pulses() == 1);
}

static void test_rollover_de_millis() {
    // millis() da la vuelta cada ~49.7 dias. La resta sin signo debe
    // seguir dando un pulso por periodo, sin tormenta de pulsos.
    Sim s(100);
    uint32_t near_wrap = 0xFFFFFFFFu - 50;
    s.t = near_wrap;
    s.step();                     // pulso #1
    uint32_t antes = s.hb.pulses();
    for (int i = 0; i < 30; i++) s.step();   // cruza el rollover
    // En 300 ms con periodo 100 ms -> ~3 pulsos nuevos, ni 0 ni 30
    uint32_t nuevos = s.hb.pulses() - antes;
    CHECK(nuevos >= 2 && nuevos <= 4);
}

static void test_corte_de_latido_deja_el_pin_en_low() {
    // Si el firmware deja de llamar update() (bloqueo del loop), el pin
    // conserva su último nivel. Si el último nivel fue LOW (caso normal
    // tras el ciclo de ancho de pulso) el PNOZ ve ausencia de pulso.
    Sim s;
    s.step();                     // HIGH
    s.step();                     // LOW  <- último estado antes del cuelgue
    CHECK(!s.hb.isHigh());
    // el cuelgue = no hay más updates; el estado sigue siendo LOW
    CHECK(!s.hb.isHigh());
}

int main() {
    std::printf("--- Latido PNOZ s4 ---\n");
    test_estado_seguro_inicial();
    test_primer_pulso_inmediato();
    test_ancho_de_pulso_un_ciclo();
    test_no_pulsa_antes_del_periodo();
    test_cadencia_en_un_segundo();
    test_reset_vuelve_a_seguro();
    test_rollover_de_millis();
    test_corte_de_latido_deja_el_pin_en_low();

    std::printf("%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (latido PNOZ)\n");
        return 0;
    }
    return 1;
}
