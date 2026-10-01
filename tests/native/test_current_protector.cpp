/**
 * @file test_current_protector.cpp
 * @brief Tests nativos (g++) de la lógica REAL del protector de corriente
 *
 * Compilar y ejecutar:  make tests-native
 * No requiere hardware ni PlatformIO - prueba el código exacto que corre en el MCU.
 */

#include "current_protector.h"

#include <cstdio>
#include <vector>

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
struct Sim {
    CurrentProtector p;
    uint32_t t = 0;
    bool relays[3] = {false, false, false};

    explicit Sim(const ProtectorConfig& cfg = ProtectorConfig()) : p(cfg) {}

    ProtectorEvent step(float amps, uint32_t dt_ms = 100) {
        t += dt_ms;
        return p.update(t, amps, relays);
    }

    // Corre total_ms y colecta todos los eventos emitidos
    std::vector<ProtectorEvent> run(float amps, uint32_t total_ms, uint32_t dt_ms = 100) {
        std::vector<ProtectorEvent> events;
        for (uint32_t el = 0; el < total_ms; el += dt_ms) {
            ProtectorEvent ev = step(amps, dt_ms);
            if (ev != ProtectorEvent::NONE) events.push_back(ev);
        }
        return events;
    }
};

static void test_corriente_normal_sin_eventos() {
    Sim s;
    s.relays[0] = true;             // Bomba 1 encendida
    auto evs = s.run(28.0f, 10000); // 28A (bomba típica) durante 10s
    CHECK(evs.empty());
    CHECK(!s.p.tripped());
    CHECK(s.p.peakAmps() >= 28.0f);
}

static void test_inrush_no_dispara_trip() {
    // Arranque de motor: pico ~60A durante 1.5s (< trip_delay 3s)
    Sim s;
    s.relays[0] = true;
    auto evs1 = s.run(60.0f, 1500);
    auto evs2 = s.run(25.0f, 3000); // vuelve a corriente normal
    bool tripped = false;
    for (auto ev : evs1) if (ev == ProtectorEvent::TRIP_OVERLOAD) tripped = true;
    for (auto ev : evs2) if (ev == ProtectorEvent::TRIP_OVERLOAD) tripped = true;
    CHECK(!tripped);
    CHECK(!s.p.tripped());
}

static void test_trip_por_sobrecarga_sostenida() {
    // 43A sostenido > 3s con bomba energizada -> TRIP exactamente una vez
    Sim s;
    s.relays[0] = true;
    auto evs = s.run(43.0f, 8000);
    int trips = 0;
    for (auto ev : evs) if (ev == ProtectorEvent::TRIP_OVERLOAD) trips++;
    CHECK(trips == 1);
    CHECK(s.p.tripped());
    // Segundo tramo en sobrecarga: ya emitido, no repite
    auto evs2 = s.run(43.0f, 5000);
    for (auto ev : evs2) CHECK(ev != ProtectorEvent::TRIP_OVERLOAD);
}

static void test_aviso_antes_del_trip() {
    // 41A (entre aviso y trip): WARN a los 1s, nunca TRIP
    Sim s;
    s.relays[0] = true;
    auto evs = s.run(41.0f, 8000);
    int warns = 0, trips = 0;
    for (auto ev : evs) {
        if (ev == ProtectorEvent::WARN_OVERLOAD) warns++;
        if (ev == ProtectorEvent::TRIP_OVERLOAD) trips++;
    }
    CHECK(warns == 1);
    CHECK(trips == 0);
}

static void test_rearm_tras_condicion_limpia() {
    // Trip -> corriente normal > reset_delay -> re-armado automático -> nuevo trip posible
    ProtectorConfig cfg;
    Sim s(cfg);
    s.relays[0] = true;
    s.run(43.0f, 5000);
    CHECK(s.p.tripped());

    s.run(20.0f, 3000);   // condición limpia > reset_delay_ms (2s)
    CHECK(!s.p.tripped()); // re-armado

    auto evs = s.run(43.0f, 5000); // nueva excursión
    int trips = 0;
    for (auto ev : evs) if (ev == ProtectorEvent::TRIP_OVERLOAD) trips++;
    CHECK(trips == 1);
}

static void test_rearm_manual() {
    Sim s;
    s.relays[0] = true;
    s.run(43.0f, 5000);
    CHECK(s.p.tripped());
    s.p.rearm(); // reset explícito de operador
    CHECK(!s.p.tripped());
}

static void test_contactor_pegado() {
    // TODOS los relés abiertos pero fluye corriente -> pegado/soldado
    Sim s;
    auto evs = s.run(3.0f, 3000);
    int welded = 0;
    for (auto ev : evs) if (ev == ProtectorEvent::CONTACTOR_WELDED) welded++;
    CHECK(welded == 1);

    // Corriente desaparece (se soldó el problema) -> re-arma para nuevo episodio
    s.run(0.0f, 2000);
    auto evs2 = s.run(3.0f, 3000);
    int welded2 = 0;
    for (auto ev : evs2) if (ev == ProtectorEvent::CONTACTOR_WELDED) welded2++;
    CHECK(welded2 == 1);
}

static void test_sin_falsa_alarma_con_rele_cerrado() {
    // Relé cerrado y corriente presente = normal, NO es "pegado"
    Sim s;
    s.relays[1] = true;
    auto evs = s.run(3.0f, 5000);
    for (auto ev : evs) CHECK(ev != ProtectorEvent::CONTACTOR_WELDED);
}

static void test_ct_fault() {
    // Sensor fuera de rango (CT desconectado -> lectura loca)
    Sim s;
    s.relays[0] = true;
    auto evs = s.run(500.0f, 2000);
    int ct = 0;
    for (auto ev : evs) if (ev == ProtectorEvent::CT_FAULT) ct++;
    CHECK(ct == 1);

    // Vuelve a rango válido -> listo para detectar de nuevo
    s.run(25.0f, 1000);
    auto evs2 = s.run(500.0f, 2000);
    int ct2 = 0;
    for (auto ev : evs2) if (ev == ProtectorEvent::CT_FAULT) ct2++;
    CHECK(ct2 == 1);
}

static void test_trip_tiene_prioridad_sobre_pegado() {
    // Relés abiertos pero corriente > trip (sobrecarga real): emite WELDED y luego TRIP
    Sim s;
    auto evs = s.run(45.0f, 8000);
    bool welded = false, tripped = false;
    for (auto ev : evs) {
        if (ev == ProtectorEvent::CONTACTOR_WELDED) welded = true;
        if (ev == ProtectorEvent::TRIP_OVERLOAD) tripped = true;
    }
    CHECK(welded);
    CHECK(tripped);
}

static void test_millwrapped() {
    // millis() desborda (uint32): el delta time unsigned no debe romperse
    Sim s;
    s.t = 0xFFFFFF00u;
    s.relays[0] = true;
    std::vector<ProtectorEvent> evs;
    for (int i = 0; i < 50; i++) {
        s.t += 100;  // wrapa a mitad de la serie
        ProtectorEvent ev = s.p.update(s.t, 43.0f, s.relays);
        if (ev != ProtectorEvent::NONE) evs.push_back(ev);
    }
    // Con wrap: transcurrieron ~5s con 43A -> debe haber trippado
    bool tripped = false;
    for (auto ev : evs) if (ev == ProtectorEvent::TRIP_OVERLOAD) tripped = true;
    CHECK(tripped);
}

int main() {
    test_corriente_normal_sin_eventos();
    test_inrush_no_dispara_trip();
    test_trip_por_sobrecarga_sostenida();
    test_aviso_antes_del_trip();
    test_rearm_tras_condicion_limpia();
    test_rearm_manual();
    test_contactor_pegado();
    test_sin_falsa_alarma_con_rele_cerrado();
    test_ct_fault();
    test_trip_tiene_prioridad_sobre_pegado();
    test_millwrapped();

    std::printf("\n%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (protector de corriente)\n");
        return 0;
    }
    return 1;
}
