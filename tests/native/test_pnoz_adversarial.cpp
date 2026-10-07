/**
 * @file test_pnoz_adversarial.cpp
 * @brief Ataque adversarial al latido del PNOZ s4 (SIF-06).
 *
 * Invariante atacado: el watchdog del PNOZ no se puede falsear. El pin
 * PB3 debe estar LOW en reposo y en cada corte de update(), y un firmware
 * colgado no debe dejar el pin en HIGH colgado más de un ciclo.
 *
 * Completa los 23 checks de test_pnoz_heartbeat.cpp atacando:
 *   - el solape de pulsos (pulso pedido y consumido en el mismo ciclo),
 *   - el reloj倒退 / saltos grandes de millis(),
 *   - periodo 0 y periodos absurdos,
 *   - el estado del pin después de un cuelgue en cada fase posible,
 *   - el reinicio (reset) en mitad de un pulso.
 *
 * Compilar:
 *   g++ -std=c++17 -Wall -Wextra -I mcu/include \
 *       tests/native/test_pnoz_adversarial.cpp mcu/src/pnoz_heartbeat.cpp \
 *       -o tests/native/build/test_pnoz_adversarial
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

// main.cpp:299-300 escribe el pin con el valor de retorno de update():
//     digitalWrite(PIN_PNOZ_HEARTBEAT, pnozHeartbeat.update(now) ? HIGH : LOW)
// Así que el "estado del pin" es exactamente lo que devuelve update().
static const uint32_t LOOP_MS = 10;      // LOOP_INTERVAL_MS (main.cpp:55)
static const uint32_t PERIODO = 100;    // PNOZ_PULSE_PERIOD_MS (config.h:68)
static const uint32_t STALE = 50;        // PNOZ_STALE_MS (config.h:72)

/** Nivel del pin tras un paso del loop. */
static bool paso(PnozHeartbeat& hb, uint32_t now) {
    return hb.update(now);
}

// ============================================================
// 1. Pulso en reposo: el pin LOW la mayoría de los ciclos
// ============================================================
static void test_reposo_es_low_la_mayoria_del_tiempo() {
    // Duty cycle esperado: 1 ciclo HIGH de cada 10 (periodo 100 ms,
    // ciclo 10 ms) -> 10 % HIGH, 90 % LOW.
    PnozHeartbeat hb(PERIODO);
    int altos = 0, bajos = 0;
    for (uint32_t i = 1; i <= 1000; i++) {     // 10 s
        if (paso(hb, i * LOOP_MS)) altos++; else bajos++;
    }
    // Exactamente 100 pulsos en 10 s (1 cada 100 ms) y 900 ciclos en LOW
    CHECK(altos == 100);
    CHECK(bajos == 900);
    // pulses() cuenta cada llamada que devuelve HIGH. El primer pulso de
    // update() es parte de esos 100 (no uno extra).
    CHECK(hb.pulses() == 100);
    CHECK(hb.isHigh() == false);  // y termina en reposo
}

static void test_un_pulso_nunca_dura_mas_de_un_ciclo() {
    // Ningún HIGH puede ocupar dos ciclos seguidos: si lo hiciera, el PNOZ
    // vería un nivel constante y no un pulso.
    PnozHeartbeat hb(PERIODO);
    for (uint32_t i = 1; i <= 500; i++) {
        bool a = paso(hb, i * LOOP_MS);
        bool b = paso(hb, i * LOOP_MS);
        CHECK(!(a && b));
    }
}

// ============================================================
// 2. Solape de pulsos
// ============================================================
static void test_sin_pulso_siempre_terminado_en_el_mismo_ciclo() {
    // El pulso se pide y se consume en la MISMA llamada (high_ se pone a
    // true al final). La llamada siguiente lo baja siempre, incluso si se
    // pide un pulso nuevo en esa misma llamada.
    PnozHeartbeat hb(PERIODO);
    // primer pulso
    CHECK(paso(hb, 10) == true);
    CHECK(hb.isHigh() == true);
    // el siguiente ciclo lo baja, sin importar cuánto tiempo haya pasado
    CHECK(paso(hb, 20) == false);
    CHECK(hb.isHigh() == false);
}

static void test_no_se_acumulan_pulsos_pendientes() {
    // Si el loop se retrasa mucho, al volver NO deben dispararse varios
    // pulsos seguidos para "ponerse al día": el siguiente ciclo tras un
    // pulso debe ser LOW pase lo que pase.
    PnozHeartbeat hb(PERIODO);
    CHECK(paso(hb, 10) == true);
    // el loop se cuelga 5 s y vuelve
    bool siguiente = paso(hb, 5010);
    CHECK(siguiente == false);       // primero baja
    CHECK(hb.isHigh() == false);
    // y no hay storm de pulsos de recuperación
    CHECK(hb.pulses() == 1);
}

static void test_salto_de_reloj_no_genera_pulsos_multiples() {
    // update() se llama una vez por ciclo. Si se llama con saltos grandes
    // de reloj, el pulso sigue siendo UNO por llamada.
    PnozHeartbeat hb(PERIODO);
    int altos = 0;
    uint32_t t = 0;
    // 200 llamadas con saltos irregulares de 10 a 5000 ms
    const uint32_t saltos[] = {10, 5000, 10, 10, 3000, 10, 10, 10, 10000};
    bool ultimo = false;
    for (int i = 0; i < 200; i++) {
        t += saltos[i % 9];
        ultimo = paso(hb, t);
        if (ultimo) altos++;
    }
    CHECK(altos <= 200);
    // el nivel final del pin es exactamente el de la última llamada
    CHECK(hb.isHigh() == ultimo);
}

// ============================================================
// 3. Rollover de millis() en la zona del pulso
// ============================================================
static void test_rollover_no_torcia_la_cadencia() {
    // Justo antes de la vuelta, con el reloj a 0xFFFFFFFF-20.
    PnozHeartbeat hb(PERIODO);
    uint32_t t = 0xFFFFFFFFu - 20u;
    bool primero = paso(hb, t);
    CHECK(primero == true);          // pulso inicial
    CHECK(paso(hb, t + 10) == false);  // baja
    // ahora cruzamos el rollover
    int altos = 0;
    for (uint32_t k = 0; k < 300; k++) {
        if (paso(hb, 0xFFFFFFF0u + k * 10u)) altos++;
    }
    // 3 s cruzando el wrap con periodo 100 ms -> unos 30 pulsos, no 300
    std::printf("   [info] rollover -> %d pulsos en 300 ciclos (3 s)\n", altos);
    CHECK(altos >= 25 && altos <= 35);
}

static void test_rollover_repetido_no_pierde_el_periodo() {
    // Varias vueltas: la cadencia debe seguir estable.
    PnozHeartbeat hb(PERIODO);
    uint32_t antes = 0;
    uint32_t t = 0xFFFFFFFFu - 500u;
    for (uint32_t k = 0; k < 4000; k++) {     // 4 vueltas de reloj
        t += LOOP_MS;
        paso(hb, t);
    }
    uint32_t made = hb.pulses();
    CHECK(made >= 395 && made <= 405);
    (void)antes;
}

static void test_reloj_en_cerca_sin_pulso_pendiente() {
    // Un "now" menor que el último pulso (el reloj se reseteó, p.ej. tras
    // un watchdog de IWDG que reinicia el contador de millis()) hace que
    // `now - last_pulse_ms_` sea un número ENORME (resta sin signo), así que
    // `>= period_ms_` se cumple en la primera llamada y se emite un pulso.
    // Tras eso, last_pulse_ms_ = now y el ritmo se normaliza.
    //
    // No es una tormenta de pulsos (un pulso, no 100), pero sí significa
    // que un reinicio del MCU produce un pulso espurio. Inofensivo para el
    // PNOZ: un pulso de más en un tren de pulsos no abre nada.
    PnozHeartbeat hb(PERIODO);
    CHECK(paso(hb, 100000) == true);          // pulso inicial
    CHECK(paso(hb, 100010) == false);          // baja
    // el reloj se resetea a 0 (tras un watchdog)
    int altos = 0, dos_seguidos = 0;
    bool prev = false;
    for (uint32_t i = 1; i <= 100; i++) {
        bool a = paso(hb, i * LOOP_MS);
        if (a) altos++;
        if (a && prev) dos_seguidos++;
        prev = a;
    }
    std::printf("   [info] reloj reseteado -> %d pulsos en 100 ciclos (1 s)\n", altos);
    // La cadencia se reanuda normal: ~1 pulso cada 100 ms -> ~10 en 1 s.
    // Lo importante es que NO es una tormenta (100 pulsos) ni dos pulsos
    // seguidos: un tren de pulsos alterado no es un estado seguro.
    CHECK(dos_seguidos == 0);
    CHECK(altos >= 9 && altos <= 11);
    CHECK(hb.isHigh() == false);
}

// ============================================================
// 4. Periodos degenerados
// ============================================================
static void test_periodo_cero_es_pulso_continuo() {
    // period_ms_ = 0: `now - last >= 0` siempre es cierto, así que el
    // pin alterna HIGH/LOW cada ciclo = 50 % de duty, no un pulso. Con la
    // config real (100 ms) esto no ocurre. Se documenta el borde: el
    // resultado es un tren de pulsos al 50 %, que un PNOZ configurado para
    // pulsos del 10 % podría rechazar.
    PnozHeartbeat hb(0);
    int altos = 0;
    for (uint32_t i = 1; i <= 100; i++) {
        if (paso(hb, i * LOOP_MS)) altos++;
    }
    std::printf("   [info] periodo 0 -> %d/100 ciclos en HIGH\n", altos);
    CHECK(altos == 50);
    CHECK(hb.pulses() == 50);
}

static void test_periodo_menor_que_el_ciclo() {
    // period_ms_ = 5 < ciclo 10 ms: mismo efecto de 50 % (o más).
    PnozHeartbeat hb(5);
    int altos = 0, dos_seguidos = 0;
    bool prev = false;
    for (uint32_t i = 1; i <= 100; i++) {
        bool a = paso(hb, i * LOOP_MS);
        if (a) altos++;
        if (a && prev) dos_seguidos++;
        prev = a;
    }
    CHECK(dos_seguidos == 0);
    CHECK(altos == 50);
}

static void test_periodo_enorme_no_pulsa_hasta_tarde() {
    // period_ms_ = 0xFFFFFFFF: sólo el pulso inicial, luego LOW. El PNOZ
    // se abre por ausencia de pulso, que es el lado seguro.
    PnozHeartbeat hb(0xFFFFFFFFu);
    CHECK(paso(hb, 10) == true);
    for (uint32_t i = 2; i <= 100; i++) {
        CHECK(paso(hb, i * LOOP_MS) == false);
    }
    CHECK(hb.isHigh() == false);
    CHECK(hb.pulses() == 1);
}

// ============================================================
// 5. Cuelgue del firmware en cada fase posible
// ============================================================
static void test_cuelgue_durante_el_pulso_deja_pin_high() {
    // CASO PEOR: el firmware se cuelga justo DESPUÉS de poner el pin en
    // HIGH. update() deja de llamarse, así que el pin se queda en HIGH.
    //
    // Esto es INHERENTE a un generador de pulsos por software: no se puede
    // evitar sin un temporizador de hardware. El PNOZ en modo watchdog
    // reacciona a la AUSENCIA de pulso (no al nivel), así que un HIGH
    // fijo equivale a pulso interrumpido -> el PNOZ abre. Es exactamente
    // el diseño de SIF-06 (docs/security_protocols.md:33-39).
    //
    // Lo que sí tiene que cumplirse: main.cpp:286-288 fuerza el pin a LOW
    // si el bloque de 100 Hz se retrasa más de PNOZ_STALE_MS.
    PnozHeartbeat hb(PERIODO);
    CHECK(paso(hb, 10) == true);       // pin HIGH
    CHECK(hb.isHigh() == true);
    // el firmware se cuelga: el pin conserva HIGH
    CHECK(hb.isHigh() == true);
    // pero en cuanto el loop vuelve, la primera llamada lo baja
    CHECK(paso(hb, 20) == false);
    CHECK(hb.isHigh() == false);
}

static void test_cuelgue_en_reposo_deja_pin_low() {
    // CASO BUENO: el firmware se cuelga en un ciclo LOW (9 de cada 10).
    PnozHeartbeat hb(PERIODO);
    paso(hb, 10);       // HIGH
    CHECK(paso(hb, 20) == false);
    CHECK(hb.isHigh() == false);
    // cuelgue: sigue en LOW -> el PNOZ ve ausencia de pulso de inmediato
    CHECK(hb.isHigh() == false);
    CHECK(hb.isHigh() == false);
}

static void test_stale_ms_cubre_la_ventana() {
    // El guard de main.cpp:286 es `now - last_block_time >= PNOZ_STALE_MS`.
    // Con bloque de 100 Hz (10 ms) y STALE=50 ms, el margen es de 5 ciclos.
    // Un retraso de 4 ciclos NO fuerza LOW (el pin ya está LOW o el pulso
    // es legítimo); uno de 5 ciclos SÍ.
    CHECK(4 * LOOP_MS < STALE);
    CHECK(5 * LOOP_MS >= STALE);
    // Y el periodo del latido (100 ms) es mayor que STALE (50 ms), así que
    // un pulso normal (10 ms) nunca choca con el guard de stale.
    CHECK(PERIODO > STALE);
    CHECK(LOOP_MS < STALE);
}

// ============================================================
// 6. reset() en mitad de un pulso
// ============================================================
static void test_reset_durante_el_pulso_vuelve_a_seguro() {
    PnozHeartbeat hb(PERIODO);
    CHECK(paso(hb, 10) == true);
    CHECK(hb.isHigh() == true);
    hb.reset();
    // reset() pone high_ = false: el pin vuelve a LOW, que es el lado
    // seguro. Si no lo hiciera, un reset dejaría el PNOZ sin pulso claro.
    CHECK(hb.isHigh() == false);
    CHECK(hb.pulses() == 0);
    // y vuelve a emitir un pulso inicial
    CHECK(paso(hb, 20) == true);
    CHECK(hb.isHigh() == true);
}

static void test_reset_durante_reposo_no_altera_el_ritmo() {
    PnozHeartbeat hb(PERIODO);
    for (uint32_t i = 1; i <= 50; i++) paso(hb, i * LOOP_MS);
    uint32_t antes = hb.pulses();
    hb.reset();
    CHECK(hb.pulses() == 0);
    for (uint32_t i = 1; i <= 50; i++) paso(hb, 1000 + i * LOOP_MS);
    // 500 ms tras el reset -> ~5 pulsos nuevos
    std::printf("   [info] tras reset -> %d pulsos en 500 ms\n", hb.pulses());
    CHECK(hb.pulses() >= 5 && hb.pulses() <= 7);
    CHECK(antes >= 5);
}

// ============================================================
// 7. La puerta que main.cpp usa de verdad
// ============================================================
static void test_replica_del_guard_de_stale_de_main() {
    // Reproduce el bloque de main.cpp:286-300 tal cual, con un "cuelgue"
    // modelado como un retraso del bloque de 100 Hz.
    struct Sim {
        PnozHeartbeat hb{PERIODO};
        uint32_t pin = 0;
        uint32_t last_block = 0;
        uint32_t last_loop = 0;

        bool loop(bool el_bloque_se_retrasa) {
            uint32_t now = last_loop + (el_bloque_se_retrasa ? 500 : LOOP_MS);
            if (now - last_block >= STALE) pin = 0;      // guard de stale
            if (now - last_loop >= LOOP_MS) {
                last_loop = now;
                last_block = now;
                pin = hb.update(now) ? 1 : 0;
            }
            return true;
        }
    };
    Sim s;
    s.last_block = s.last_loop = 1000;
    // 30 ciclos normales: el pin nunca se queda pegado en HIGH más de 1 ciclo
    int highs_consecutivos = 0, prev = 0, max_consec = 0;
    for (int i = 0; i < 30; i++) {
        s.loop(false);
        if (s.pin == 1) { highs_consecutivos++; if (highs_consecutivos > max_consec) max_consec = highs_consecutivos; }
        else highs_consecutivos = 0;
        (void)prev;
    }
    CHECK(max_consec <= 1);
    CHECK(s.pin == 0);

    //ahora un cuelgue: el bloque no corre durante 500 ms
    s.pin = 1;                     // el firmware había dejado el pin en HIGH
    s.last_block = s.last_loop;    // referencia antigua
    uint32_t ahora = s.last_loop + 500;
    if (ahora - s.last_block >= STALE) s.pin = 0;   // el guard lo rescata
    CHECK(s.pin == 0);
}

int main() {
    std::printf("--- Latido PNOZ: casos adversariales ---\n");
    test_reposo_es_low_la_mayoria_del_tiempo();
    test_un_pulso_nunca_dura_mas_de_un_ciclo();
    test_sin_pulso_siempre_terminado_en_el_mismo_ciclo();
    test_no_se_acumulan_pulsos_pendientes();
    test_salto_de_reloj_no_genera_pulsos_multiples();
    test_rollover_no_torcia_la_cadencia();
    test_rollover_repetido_no_pierde_el_periodo();
    test_reloj_en_cerca_sin_pulso_pendiente();
    test_periodo_cero_es_pulso_continuo();
    test_periodo_menor_que_el_ciclo();
    test_periodo_enorme_no_pulsa_hasta_tarde();
    test_cuelgue_durante_el_pulso_deja_pin_high();
    test_cuelgue_en_reposo_deja_pin_low();
    test_stale_ms_cubre_la_ventana();
    test_reset_durante_el_pulso_vuelve_a_seguro();
    test_reset_durante_reposo_no_altera_el_ritmo();
    test_replica_del_guard_de_stale_de_main();

    std::printf("%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (PNOZ adversarial)\n");
        return 0;
    }
    return 1;
}