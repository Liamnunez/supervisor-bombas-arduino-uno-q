/**
 * @file test_trip_policy.cpp
 * @brief Tests nativos (g++) de la política de reintentos del trip (SIF-04)
 *
 * Compilar y ejecutar:  make tests-native
 * Sin hardware. Prueba el código exacto que decide si el sistema se
 * recupera solo o espera a una persona.
 */

#include "trip_policy.h"

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

static TripPolicyConfig cfg3() {
    // 3 intentos / 15 min / cooldown 60 s / 5 A
    TripPolicyConfig c;
    c.max_attempts = 3;
    c.window_ms = 900000;
    c.cooldown_ms = 60000;
    c.recover_amps = 5.0f;
    return c;
}

// --- Escenario completo de tormenta: 3 trips y parada ---
static void test_ciclo_completo_3_trips() {
    TripPolicy p(cfg3());

    // 1er trip: el protector ya abrió los relés, la política no decide nada
    CHECK(p.update(1000, true, 60.0f) == TripDecision::NONE);
    CHECK(p.attempts() == 1);
    CHECK(p.remaining() == 2);
    CHECK(!p.latched());

    // Durante el enfriamiento no hace nada
    CHECK(p.update(30000, false, 0.0f) == TripDecision::NONE);
    CHECK(p.update(59000, false, 0.0f) == TripDecision::NONE);

    // Vencido el enfriamiento y corriente limpia -> recupera solo
    CHECK(p.update(61000, false, 0.0f) == TripDecision::RECOVER);

    // 2º trip
    CHECK(p.update(70000, true, 60.0f) == TripDecision::NONE);
    CHECK(p.attempts() == 2);
    CHECK(p.remaining() == 1);

    CHECK(p.update(130000, false, 0.0f) == TripDecision::RECOVER);

    // 3er trip -> HOLD, una sola vez
    CHECK(p.update(140000, true, 60.0f) == TripDecision::HOLD);
    CHECK(p.latched());
    CHECK(p.attempts() == 3);
    CHECK(p.remaining() == 0);

    // Después del HOLD no vuelve a spamear HOLD
    for (uint32_t t = 141000; t < 160000; t += 10000) {
        CHECK(p.update(t, false, 0.0f) == TripDecision::NONE);
    }
    // Ni siquiera con un trip nuevo (aunque no debería ocurrir)
    CHECK(p.update(200000, true, 60.0f) == TripDecision::NONE);
    CHECK(p.attempts() == 3);
    CHECK(p.latched());
}

static void test_recover_es_edge_no_estado() {
    TripPolicy p(cfg3());
    p.update(0, true, 60.0f);
    CHECK(p.update(60000, false, 0.0f) == TripDecision::RECOVER);
    // Sin nuevo trip no debe repetir RECOVER cada ciclo de 100Hz
    CHECK(p.update(60100, false, 0.0f) == TripDecision::NONE);
    CHECK(p.update(61000, false, 0.0f) == TripDecision::NONE);
    CHECK(p.update(119900, false, 0.0f) == TripDecision::NONE);
    CHECK(p.update(120000, false, 0.0f) == TripDecision::RECOVER);
}

static void test_no_recovera_con_corriente_sucia() {
    TripPolicy p(cfg3());
    p.update(0, true, 60.0f);
    // Cooldown vencido pero la bomba sigue consumiendo: no re-armar,
    // re-armar en corto no protege nada
    for (uint32_t t = 60000; t < 120000; t += 10000) {
        CHECK(p.update(t, false, 25.0f) == TripDecision::NONE);
    }
    // Cuando por fin baja, recupera
    CHECK(p.update(180000, false, 0.0f) == TripDecision::RECOVER);
}

static void test_ventana_de_15_min_poda() {
    TripPolicy p(cfg3());
    p.update(0, true, 60.0f);          // intento 1 en t=0
    CHECK(p.attempts() == 1);
    // 20 min después (fuera de la ventana de 15 min) el intento caduca
    p.update(1200000, false, 0.0f);
    CHECK(p.attempts() == 0);
    // Un trip nuevo cuenta como el primero, no como el segundo
    p.update(1201000, true, 60.0f);
    CHECK(p.attempts() == 1);
    CHECK(!p.latched());
}

static void test_trips_espaciados_no_latchean() {
    TripPolicy p(cfg3());
    // 3 trips separados por más de la ventana de 15 min: nunca agota
    p.update(0, true, 60.0f);
    CHECK(p.attempts() == 1);
    // 20 min después el intento caducó: no hay nada pendiente que recuperar
    CHECK(p.update(1200000, false, 0.0f) == TripDecision::NONE);
    CHECK(p.attempts() == 0);

    // Nuevo trip cuenta como el primero y recupera normal
    p.update(1800000, true, 60.0f);
    CHECK(p.attempts() == 1);
    CHECK(p.update(1860000, false, 0.0f) == TripDecision::RECOVER);

    // Y el tercero, otro cuarto de hora después, tampoco agota intentos
    p.update(3600000, true, 60.0f);
    CHECK(!p.latched());
    CHECK(p.attempts() == 1);
}

static void test_operator_reset_limpia() {
    TripPolicy p(cfg3());
    p.update(0, true, 60.0f);
    p.update(60000, false, 0.0f);
    p.update(70000, true, 60.0f);
    p.update(130000, false, 0.0f);
    CHECK(p.update(140000, true, 60.0f) == TripDecision::HOLD);
    CHECK(p.latched());

    p.operatorReset();
    CHECK(!p.latched());
    CHECK(p.attempts() == 0);
    CHECK(p.remaining() == 3);
    // Tras el reset hay que esperar cooldown antes de recuperar
    CHECK(p.update(141000, false, 0.0f) == TripDecision::NONE);

    // Y el ciclo de 3 intentos vuelve a empezar
    p.update(200000, true, 60.0f);
    CHECK(p.attempts() == 1);
}

static void test_retry_in_ms() {
    TripPolicy p(cfg3());
    CHECK(p.retryInMs(0) == 0);            // sin intentos: no hay espera
    p.update(1000, true, 60.0f);
    CHECK(p.retryInMs(1000) == 60000);
    CHECK(p.retryInMs(31000) == 30000);
    CHECK(p.retryInMs(61000) == 0);
}

static void test_umbral_de_ampereaje() {
    TripPolicyConfig c = cfg3();
    TripPolicy p(c);
    p.update(0, true, 60.0f);
    // Justo en el umbral (5.0 A) sí recupera: el CT no es perfecto y
    // quedarse a 0.01 A del umbral no debe bloquear el re-arm
    CHECK(p.update(60000, false, 5.0f) == TripDecision::RECOVER);

    TripPolicy p2(c);
    p2.update(0, true, 60.0f);
    CHECK(p2.update(60000, false, 5.1f) == TripDecision::NONE);
}

static void test_un_solo_intento_permitido() {
    TripPolicyConfig c;
    c.max_attempts = 1;
    c.window_ms = 900000;
    c.cooldown_ms = 1000;
    c.recover_amps = 5.0f;
    TripPolicy p(c);
    // Con max=1 el primer trip ya agota: no re-arma nunca solo
    CHECK(p.update(0, true, 60.0f) == TripDecision::HOLD);
    CHECK(p.latched());
}

static void test_max_attempts_limitado_por_slots() {
    TripPolicyConfig c;
    c.max_attempts = 200;     // absurdo a propósito
    TripPolicy p(c);
    // No debe desbordar attempt_ms_[TRIP_MAX_SLOTS]
    for (int i = 0; i < 20; i++) {
        p.update(static_cast<uint32_t>(i) * 1000, true, 60.0f);
    }
    CHECK(p.attempts() <= TRIP_MAX_SLOTS);
    CHECK(p.latched());
}

static void test_rollover_de_millis() {
    TripPolicy p(cfg3());
    uint32_t near_wrap = 0xFFFFFFFFu - 100000;
    // 1er trip antes de la vuelta
    CHECK(p.update(near_wrap, true, 60.0f) == TripDecision::NONE);
    CHECK(p.attempts() == 1);
    // Recupera cruzando el rollover (60 s después)
    CHECK(p.update(near_wrap + 60000, false, 0.0f) == TripDecision::RECOVER);
    CHECK(p.attempts() == 1);
    // La ventana tampoco debe romperse con la vuelta
    p.update(near_wrap + 120000, true, 60.0f);
    CHECK(p.attempts() == 2);
    CHECK(!p.latched());
}

static void test_sin_trip_no_hace_nada() {
    TripPolicy p(cfg3());
    for (uint32_t t = 0; t < 1000000; t += 10000) {
        CHECK(p.update(t, false, 0.0f) == TripDecision::NONE);
    }
    CHECK(p.attempts() == 0);
    CHECK(!p.latched());
}

static void test_config_por_defecto() {
    TripPolicy p;   // sin config explícita
    CHECK(p.config().max_attempts == 3);
    CHECK(p.config().window_ms == 900000);
    CHECK(p.config().cooldown_ms == 60000);
    CHECK(p.update(0, true, 60.0f) == TripDecision::NONE);
    CHECK(p.update(60000, false, 0.0f) == TripDecision::RECOVER);
}

int main() {
    std::printf("--- Política de trip SIF-04 ---\n");
    test_ciclo_completo_3_trips();
    test_recover_es_edge_no_estado();
    test_no_recovera_con_corriente_sucia();
    test_ventana_de_15_min_poda();
    test_trips_espaciados_no_latchean();
    test_operator_reset_limpia();
    test_retry_in_ms();
    test_umbral_de_ampereaje();
    test_un_solo_intento_permitido();
    test_max_attempts_limitado_por_slots();
    test_rollover_de_millis();
    test_sin_trip_no_hace_nada();
    test_config_por_defecto();

    std::printf("%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (política de trip)\n");
        return 0;
    }
    return 1;
}
