/**
 * @file test_trip_policy_adversarial.cpp
 * @brief Ataque adversarial a la política de trip (SIF-04) - casos que los
 *        173 checks de test_trip_policy.cpp NO cubren.
 *
 * Invariante atacado: la política de trip NO se puede saltar. Es decir, un
 *Thirdparty no puede conseguir que el sistema re-arme el protector de
 * sobrecarga cuando no toca, ni con configuraciones degeneradas, ni
 * desbordando el histórico circular, ni con millis() en la vuelta.
 *
 * Compilar:
 *   g++ -std=c++17 -Wall -Wextra -I mcu/include \
 *       tests/native/test_trip_policy_adversarial.cpp mcu/src/trip_policy.cpp \
 *       -o tests/native/build/test_trip_policy_adversarial
 */

#include "trip_policy.h"

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

// Configuración nominal del firmware (main.cpp:63-70 -> config.h:136-139)
static const uint8_t TRIP_MAX_ATTEMPTS_SIM = 3;

static TripPolicyConfig nominal() {
    TripPolicyConfig c;
    c.max_attempts = TRIP_MAX_ATTEMPTS_SIM;
    c.window_ms    = 900000;
    c.cooldown_ms  = 60000;
    c.recover_amps = 5.0f;
    return c;
}

// ============================================================
// 1. Overflow de attempt_ms_: muchos trips en el MISMO instante
// ============================================================
static void test_diez_mil_trips_en_el_mismo_milis() {
    // update() con trip_now=true durante muchos ciclos con el reloj congelado.
    // attempt_ms_ tiene TRIP_MAX_SLOTS (8) casillas: el guard
    // `if (attempts_ < TRIP_MAX_SLOTS)` (trip_policy.cpp:54) debe impedir
    // la escritura fuera del array.
    TripPolicy p(nominal());
    for (int i = 0; i < 10000; i++) {
        p.update(1000, true, 60.0f);
    }
    CHECK(p.attempts() <= TRIP_MAX_SLOTS);
    CHECK(p.attempts() == 3);        // se latcheó al llegar a max_attempts
    CHECK(p.latched());
    // Una vez latcheado, ni 10000 trips más lo_deslatchean
    for (int i = 0; i < 10000; i++) {
        p.update(1000, true, 60.0f);
    }
    CHECK(p.latched());
    CHECK(p.attempts() == 3);
}

// ============================================================
// 2. max_attempts mayor que TRIP_MAX_SLOTS
// ============================================================
static void test_max_attempts_absurdo_no_desborda() {
    TripPolicyConfig c;
    c.max_attempts = 255;
    c.window_ms = 900000;
    c.cooldown_ms = 1000;
    c.recover_amps = 5.0f;
    TripPolicy p(c);

    // maxAttempts() recorta a TRIP_MAX_SLOTS, así que con 255 el latch
    // debería llegar alfilling el histórico, no antes.
    for (uint8_t i = 1; i <= TRIP_MAX_SLOTS; i++) {
        p.update(static_cast<uint32_t>(i) * 100000, true, 60.0f);
    }
    CHECK(p.attempts() <= TRIP_MAX_SLOTS);
    CHECK(p.latched());     // con 255 pedidos, agota los 8 huecos
    CHECK(p.remaining() == 0);

    // Y el histórico no se ha corrompido: tras operatorReset() arranca limpio
    p.operatorReset();
    CHECK(p.attempts() == 0);
    CHECK(p.update(1000000, true, 60.0f) == TripDecision::NONE);
    CHECK(p.attempts() == 1);
    CHECK(!p.latched());
}

// max_attempts = 1 y = 2: bordes
static void test_max_attempts_bordes() {
    for (uint8_t ma = 1; ma <= 3; ma++) {
        TripPolicyConfig c;
        c.max_attempts = ma;
        c.window_ms = 900000;
        c.cooldown_ms = 1000;
        c.recover_amps = 5.0f;
        TripPolicy p(c);
        TripDecision d = TripDecision::NONE;
        for (uint8_t i = 0; i < ma; i++) {
            d = p.update(i * 100000, true, 60.0f);
        }
        CHECK(p.attempts() == ma);
        CHECK(p.latched());
        CHECK(d == TripDecision::HOLD);
    }
}

// max_attempts = 0 -> maxAttempts() lo sube a 1
static void test_max_attempts_cero_no_deja_rearmar() {
    TripPolicyConfig c;
    c.max_attempts = 0;      // degenerado
    c.window_ms = 900000;
    c.cooldown_ms = 1000;
    c.recover_amps = 5.0f;
    TripPolicy p(c);
    CHECK(p.update(0, true, 60.0f) == TripDecision::HOLD);
    CHECK(p.latched());
    // Con cooldown vencido y corriente limpia NO debe recuperar solo
    CHECK(p.update(100000, false, 0.0f) == TripDecision::NONE);
}

// ============================================================
// 3. window_ms = 0 y cooldown_ms = 0
// ============================================================
static void test_window_cero_puede_romper_el_latch() {
    // window_ms = 0 hace que prune() descarte los intentos en CADA ciclo
    // (antiguedad <= 0 sólo se cumple en el mismo milisegundo). Entonces
    // attempts_ vuelve a 0 y la cuenta de 3 intentos nunca se agota:
    // el protector podría re-armar indefinidamente.
    //
    // NO es un bug alcanzable con la config del firmware (window=900000),
    // pero sí es un fallo de robustez del módulo: una config degenerada
    // desactiva la protección de "esperar a una persona".
    TripPolicyConfig c;
    c.max_attempts = 3;
    c.window_ms = 0;
    c.cooldown_ms = 1000;
    c.recover_amps = 5.0f;
    TripPolicy p(c);

    int recoveries = 0;
    // Cada "tormenta": trip, luego cooldown, luego RECOVER
    for (int ronda = 0; ronda < 10; ronda++) {
        uint32_t t = ronda * 100000u;
        p.update(t, true, 60.0f);            // trip
        p.update(t + 50000, false, 0.0f);    // cooldown + corriente limpia
        if (p.update(t + 60000, false, 0.0f) == TripDecision::RECOVER) recoveries++;
    }
    // Con window_ms = 0 el latch NO se alcanza nunca -> recoveries == 10.
    // La invariante que queremos es: nunca más de max_attempts-1 = 2
    // auto-recuperaciones sin intervención del operador.
    std::printf("   [info] window_ms=0 -> %d auto-recuperaciones en 10 tormentas\n",
                recoveries);
    CHECK(recoveries <= 2);
}

static void test_cooldown_cero_recover_repetido() {
    // cooldown_ms = 0: cooldown_until_ = now + 0 = now, así que la
    // condición de enfriamiento se cumple en el mismo ciclo. RECOVER se
    // devuelve, se recalcula cooldown_until_ = now... y vuelve a cumplirse.
    // El caller (main.cpp:358) llama a autoRecoverTrip() + rearm() en cada
    // RECOVER. Con cooldown 0 eso es un re-arm por ciclo de 100 Hz.
    TripPolicyConfig c;
    c.max_attempts = 3;
    c.window_ms = 900000;
    c.cooldown_ms = 0;
    c.recover_amps = 5.0f;
    TripPolicy p(c);

    p.update(0, true, 60.0f);            // intento 1
    int recovers = 0;
    for (int i = 0; i < 100; i++) {
        if (p.update(i * 10, false, 0.0f) == TripDecision::RECOVER) recovers++;
    }
    std::printf("   [info] cooldown_ms=0 -> %d RECOVER en 100 ciclos\n", recovers);
    // Con la config del firmware (TRIP_COOLDOWN_MS=60000) esto da 0 RECOVER.
    // Con cooldown 0 el módulo devuelve un RECOVER POR CICLO, lo que en
    // main.cpp:358 significa un re-arm del protector de sobrecarga a 100 Hz
    // sin ningún enfriamiento real: exactamente el "re-armar en corto" que
    // el comentario de la línea 75-76 dice querer evitar.
    CHECK(recovers <= 1);
}

static void test_cooldown_cero_con_ventana_nominal_no_latchea() {
    // Combinación: cooldown 0 pero ventana nominal. Cada trip cuenta (la
    // ventana no poda), así que a los 3 intentos debe latchear.
    TripPolicyConfig c;
    c.max_attempts = 3;
    c.window_ms = 900000;
    c.cooldown_ms = 0;
    c.recover_amps = 5.0f;
    TripPolicy p(c);
    p.update(0, true, 60.0f);
    p.update(100, true, 60.0f);
    TripDecision d = p.update(200, true, 60.0f);
    CHECK(p.attempts() == 3);
    CHECK(p.latched());
    CHECK(d == TripDecision::HOLD);
}

// ============================================================
// 4. recover_amps negativo
// ============================================================
static void test_recover_amps_negativo() {
    // recover_amps < 0 es imposible en la config real (5.0f), pero:
    // `if (amps > cfg_.recover_amps)` con recover_amps negativo y amps >= 0
    // NUNCA se cumple -> nunca RECOVER -> el sistema se queda parado
    // esperando a un operador. Fail-safe pero.available.
    TripPolicyConfig c;
    c.max_attempts = 3;
    c.window_ms = 900000;
    c.cooldown_ms = 1000;
    c.recover_amps = -1.0f;
    TripPolicy p(c);
    p.update(0, true, 60.0f);
    for (uint32_t t = 1000; t < 200000; t += 1000) {
        TripDecision d = p.update(t, false, 0.0f);
        CHECK(d != TripDecision::RECOVER);
    }
    CHECK(!p.latched());     // NO debe latchear: simplemente no re-arma
    CHECK(p.attempts() == 1);
}

static void test_recover_amps_cero_exige_cero_amps() {
    // recover_amps = 0.0: sólo recupera con corriente EXACTAMENTE 0.
    // Con un pelo de corriente no recupera -> fail-safe.
    TripPolicyConfig c;
    c.max_attempts = 3;
    c.window_ms = 900000;
    c.cooldown_ms = 1000;
    c.recover_amps = 0.0f;
    TripPolicy p(c);
    p.update(0, true, 60.0f);
    CHECK(p.update(10000, false, 0.1f) == TripDecision::NONE);
    CHECK(p.update(20000, false, 0.0f) == TripDecision::RECOVER);
}

// recover_amps altísimo: recupera aunque la bomba esté consumiendo
static void test_recover_amps_enorme_rearma_en_corto() {
    // Si alguien configurase recover_amps por encima de la corriente de
    // una bomba (~30 A), el re-arm sería "en corto": se cerraría el relé
    // con la bomba ya consumiendo y el protector no vería la rampa. Con la
    // config real (5.0f) esto no ocurre; el test documenta el borde.
    TripPolicyConfig c;
    c.max_attempts = 3;
    c.window_ms = 900000;
    c.cooldown_ms = 1000;
    c.recover_amps = 1e9f;
    TripPolicy p(c);
    p.update(0, true, 60.0f);
    // 60 A de bomba sigue "por debajo" del umbral -> recupera
    CHECK(p.update(10000, false, 60.0f) == TripDecision::RECOVER);
}

// ============================================================
// 5. millis() cerca del rollover
// ============================================================
static void test_rollover_cooldown_pendiente() {
    // Cooldown abierto justo antes de la vuelta de millis(): el RECOVER
    // debe llegar igual. Usa la resta con signo de alcanzo().
    TripPolicy p(nominal());
    uint32_t cerca = 0xFFFFFFFFu - 30000u;   // cooldown_until_ se desborda
    p.update(cerca, true, 60.0f);
    CHECK(p.attempts() == 1);
    // 60 s después (cruzando el rollover)
    TripDecision d = p.update(cerca + 60000u, false, 0.0f);
    CHECK(d == TripDecision::RECOVER);
}

static void test_rollover_prune_no_poda_de_mas() {
    // Intentos justo antes de la vuelta: prune() con antigüedad unsigned
    // no debe perderlos ni contarlos como viejos.
    TripPolicy p(nominal());
    uint32_t cerca = 0xFFFF0000u;
    p.update(cerca, true, 60.0f);
    p.update(cerca + 1000, true, 60.0f);
    CHECK(p.attempts() == 2);
    // Cruzando el rollover, los intentos siguen dentro de la ventana
    p.update(cerca + 2000, false, 60.0f);
    CHECK(p.attempts() == 2);
}

static void test_rollover_cooldown_ya_vencido() {
    TripPolicy p(nominal());
    // Cooldown abierto en el pasado lejano relative al wrap
    uint32_t t0 = 0xFFFFFFFFu - 100000u;
    p.update(t0, true, 60.0f);
    // Un now que "retrocede" en la aritmética con signo no debe exploitar
    // un cooldown inexistente. release: cooldown_until_ = t0+60000, que
    // aún no se ha alcanzado.
    CHECK(p.update(t0 + 1000, false, 0.0f) == TripDecision::NONE);
    CHECK(p.update(t0 + 60000, false, 0.0f) == TripDecision::RECOVER);
}

static void test_ventana_mayor_que_el_reloj() {
    // window_ms = UINT32_MAX: nada caduca nunca. Intentos acumulados
    // durante "eternidad" -> debe latchear igual (8 slots).
    TripPolicyConfig c;
    c.max_attempts = 3;
    c.window_ms = 0xFFFFFFFFu;
    c.cooldown_ms = 1000;
    c.recover_amps = 5.0f;
    TripPolicy p(c);
    for (uint32_t t = 0; t < 300000; t += 50000) {
        p.update(t, true, 60.0f);
    }
    CHECK(p.latched());
    CHECK(p.attempts() == 3);
}

// ============================================================
// 6. RECOVER no se repite antes de tiempo / no se repite sin nuevo trip
// ============================================================
static void test_recover_es_edge_con_config_nominal() {
    // El caso base que SÍ debe cumplirse siempre (config de firmware):
    // RECOVER es un flanco, no un estado.
    //
    // Matiz importante: con corriente limpia y sin relé que cierre, la
    // política VUELVE a proponer RECOVER cada cooldown_ms. Eso es
    // intencionado ("RECOVER consume un turno de enfriamiento",
    // trip_policy.cpp:83-86) y es inocuo: en el caso real, en cuanto el
    // relé cierra la corriente sube y el guard de amps (línea 77) corta
    // el re-arm. La invariante que sí tiene que cumplirse es la
    // SEPARACIÓN: nunca dos RECOVER más cercanos que cooldown_ms.
    TripPolicy p(nominal());
    p.update(0, true, 60.0f);
    int n = 0;
    uint32_t ultimo = 0;
    bool demasiado_junto = false;
    for (uint32_t t = 0; t < 600000; t += 10) {
        if (p.update(t, false, 0.0f) == TripDecision::RECOVER) {
            if (n > 0 && (t - ultimo) < 60000) demasiado_junto = true;
            ultimo = t;
            n++;
        }
    }
    std::printf("   [info] config nominal -> %d RECOVER en 600 s (separados >= 60 s)\n", n);
    CHECK(n >= 1);                    // al menos uno: se recuperó solo
    CHECK(!demasiado_junto);          // y nunca dos seguidos
    // Con la corriente de una bomba real encendida, no debe re-armar:
    TripPolicy p2(nominal());
    p2.update(0, true, 60.0f);
    int n2 = 0;
    for (uint32_t t = 0; t < 600000; t += 10) {
        if (p2.update(t, false, 30.0f) == TripDecision::RECOVER) n2++;
    }
    CHECK(n2 == 0);                   // 30 A > 5 A de umbral: nunca re-arma
}

static void test_no_recover_mientras_hay_trip_pendiente() {
    // Si el protector vuelve a emitir TRIP_OVERLOAD este ciclo,
    // update() debe contar el intento y NO devolver RECOVER.
    TripPolicy p(nominal());
    p.update(0, true, 60.0f);
    p.update(60000, false, 0.0f);         // RECOVER
    p.update(60010, true, 60.0f);         // intento 2
    CHECK(p.attempts() == 2);
    CHECK(!p.latched());
    // Re-trip inmediato: cuenta y, al ser el 3º de 3, LATCHEA (HOLD).
    // No devuelve RECOVER: sigue sin re-armar.
    CHECK(p.update(60020, true, 60.0f) == TripDecision::HOLD);
    CHECK(p.attempts() == 3);
    CHECK(p.latched());
    // Y ya latcheado, ni aunque la corriente se limpie vuelve a re-armar
    CHECK(p.update(200000, false, 0.0f) == TripDecision::NONE);
}

static void test_latch_no_se_abre_solo_al_vencer_ventana() {
    TripPolicy p(nominal());
    p.update(0, true, 60.0f);
    p.update(60000, false, 0.0f);
    p.update(60010, true, 60.0f);
    p.update(70000, false, 0.0f);
    CHECK(p.update(80000, true, 60.0f) == TripDecision::HOLD);
    CHECK(p.latched());
    // Mucho después, los intentos caducan pero el latch persiste:
    // operatorReset() es la ÚNICA puerta.
    for (uint32_t t = 900000; t < 1200000; t += 60000) {
        CHECK(p.update(t, false, 0.0f) == TripDecision::NONE);
    }
    CHECK(p.latched());
    CHECK(p.attempts() == 0);     // podados, pero sigue latcheado
    // Ni siquiera un TRIP nuevo lo re-arma
    CHECK(p.update(1300000, true, 60.0f) == TripDecision::NONE);
    CHECK(p.latched());
    // Sólo el operador
    p.operatorReset();
    CHECK(!p.latched());
    CHECK(p.update(1300010, false, 0.0f) == TripDecision::NONE);
}

static void test_hold_se_reporta_una_sola_vez() {
    // main.cpp manda un alert cuando recibe HOLD (ERR_TRIP_LATCH). Si HOLD
    // se repitiera a 100 Hz llenaría Telegram de críticos.
    TripPolicy p(nominal());
    p.update(0, true, 60.0f);
    p.update(10000, true, 60.0f);
    int holds = 0;
    for (uint32_t t = 20000; t < 60000; t += 10) {
        if (p.update(t, true, 60.0f) == TripDecision::HOLD) holds++;
    }
    CHECK(holds == 1);
}

static void test_retry_in_ms_no_underflow() {
    TripPolicy p(nominal());
    p.update(1000, true, 60.0f);
    // retryInMs debe estar en [0, cooldown]
    for (uint32_t t = 1000; t < 200000; t += 5000) {
        uint32_t r = p.retryInMs(t);
        CHECK(r <= 60000);
    }
    CHECK(p.retryInMs(1000) == 60000);
    CHECK(p.retryInMs(61000) == 0);
}

static void test_remaining_no_underflow() {
    TripPolicyConfig c;
    c.max_attempts = 1;
    c.window_ms = 900000;
    c.cooldown_ms = 1000;
    c.recover_amps = 5.0f;
    TripPolicy p(c);
    p.update(0, true, 60.0f);
    CHECK(p.latched());
    CHECK(p.remaining() == 0);      // no 255 ni negativo
    CHECK(p.retryInMs(100000) == 0);
}

int main() {
    std::printf("--- Política de trip: casos adversariales ---\n");
    test_diez_mil_trips_en_el_mismo_milis();
    test_max_attempts_absurdo_no_desborda();
    test_max_attempts_bordes();
    test_max_attempts_cero_no_deja_rearmar();
    test_window_cero_puede_romper_el_latch();
    test_cooldown_cero_recover_repetido();
    test_cooldown_cero_con_ventana_nominal_no_latchea();
    test_recover_amps_negativo();
    test_recover_amps_cero_exige_cero_amps();
    test_recover_amps_enorme_rearma_en_corto();
    test_rollover_cooldown_pendiente();
    test_rollover_prune_no_poda_de_mas();
    test_rollover_cooldown_ya_vencido();
    test_ventana_mayor_que_el_reloj();
    test_recover_es_edge_con_config_nominal();
    test_no_recover_mientras_hay_trip_pendiente();
    test_latch_no_se_abre_solo_al_vencer_ventana();
    test_hold_se_reporta_una_sola_vez();
    test_retry_in_ms_no_underflow();
    test_remaining_no_underflow();

    std::printf("%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (trip adversarial)\n");
        return 0;
    }
    return 1;
}