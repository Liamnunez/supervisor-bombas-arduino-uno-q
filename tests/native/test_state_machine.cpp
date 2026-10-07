/**
 * @file test_state_machine.cpp
 * @brief Tests nativos (g++) de la lógica REAL de la máquina de estados.
 *
 * `mcu/src/state_machine.cpp` decide si una bomba puede arrancar. No tenía
 * NINGUNA cobertura nativa (tests/test_state_machine.py prueba un
 * MockStateMachine definido dentro del propio test, no el firmware). Estos
 * tests compilan el .cpp de verdad contra un doble de Arduino.h
 * (tests/native/arduino_fake.h) y ejercitan los caminos reales, incluidos
 * los relés físicos.
 *
 * Compilar:
 *   g++ -std=c++17 -Wall -Wextra -I mcu/include -I tests/native/fake_arduino \
 *       tests/native/test_state_machine.cpp \
 *       mcu/src/state_machine.cpp mcu/src/relay_control.cpp \
 *       -o tests/native/build/test_state_machine
 *
 * Qué invariantes intenta ROMPER:
 *   1. El límite de 1 bomba en GENERADOR (SIF-01) es infranqueable.
 *   2. Nadie auto-recupera un fault de feedback.
 *   7. El cable cortado da el estado restrictivo.
 *
 * Todos los tests son deterministas: el reloj lo mueve el test, nunca el
 * hardware real.
 */

#include "Arduino.h"   // el doble de test (vía -I tests/native/fake_arduino)

#include "config.h"
#include "state_machine.h"
#include "relay_control.h"

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

// ============================================================
// Andamiaje: un ciclo del bloque de 100 Hz de main.cpp
// ============================================================

static const uint32_t CICLO_MS = 10;   // LOOP_INTERVAL_MS (main.cpp:55)

static const int PINES_PLC[NUM_BOMBAS]  = {PIN_PLC_BOMBA1, PIN_PLC_BOMBA2, PIN_PLC_BOMBA3};
static const int PINES_FB[NUM_BOMBAS]   = {PIN_FEEDBACK_B1, PIN_FEEDBACK_B2, PIN_FEEDBACK_B3};
static const int PINES_RELE[NUM_BOMBAS] = {PIN_RELE_BOMBA1, PIN_RELE_BOMBA2, PIN_RELE_BOMBA3};

// El reloj nunca retrocede entre tests: handleEmergencia() y
// handleMantenimiento() guardan el instante del último parpadeo en una
// variable static de función y compararían contra un reloj menor.
static uint32_t g_base_reloj = 1000000u;

static void nuevoTest() {
    fake::reset();
    g_base_reloj += 100000u;
    fake::setMillis(g_base_reloj);
}

/**
 * Un ciclo del loop principal: main.cpp:290-312 hace exactamente esto
 * (avanza el reloj, stateMachine.update(), relayControl.apply(stateMachine)).
 * Los relés están en el bucle a propósito: una invariante que sólo se
 * respeta en el estado interno pero no llega al pin no es una invariante.
 */
static void tick(StateMachine& sm, RelayControl& rl) {
    fake::advance(CICLO_MS);
    sm.update();
    rl.apply(sm);
}

/** Ciclo sin relés (para tests que sólo miran la lógica). */
static void tick(StateMachine& sm) {
    fake::advance(CICLO_MS);
    sm.update();
}

static void ticks(StateMachine& sm, int n) {
    for (int i = 0; i < n; i++) tick(sm);
}

static void ticks(StateMachine& sm, RelayControl& rl, int n) {
    for (int i = 0; i < n; i++) tick(sm, rl);
}

static int bombsCerradas(const StateMachine& sm) {
    return (sm.getRelayClosed(0) ? 1 : 0) +
           (sm.getRelayClosed(1) ? 1 : 0) +
           (sm.getRelayClosed(2) ? 1 : 0);
}

/**
 * Arranque equivalente al setup() de main.cpp:212-265.
 * OJO: StateMachine NO configura el pin de modo por sí mismo (nadie llama a
 * pinMode(PIN_MODO_GEN) dentro de state_machine.cpp). El INPUT_PULLUP lo
 * pone modeDetect.begin() (mode_detect.cpp:16) y el INPUT_PULLDOWN de los
 * feedbacks feedback.begin() (feedback.cpp:23), y setup() los llama ANTES
 * que stateMachine.begin() (main.cpp:248 vs 259). El test reproduce ese
 * orden; test_state_machine_configura_su_propio_pin_loComprueba().
 *
 * @param pin_modo_generador true = PC0 en LOW = el contacto seco dice
 *                            GENERADOR; false = PC0 en HIGH = RED.
 */
static void arranque(StateMachine& sm, RelayControl& rl, bool pin_modo_generador) {
    // Pines de relé: OUTPUT y abiertos (main.cpp:214-219)
    for (int i = 0; i < NUM_BOMBAS; i++) {
        fake::setMode(PINES_RELE[i], OUTPUT);
        fake::setPin(PINES_RELE[i], RELAY_INACTIVE_LEVEL);
    }
    // Pines de entrada, con su pull
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    for (int i = 0; i < NUM_BOMBAS; i++) {
        fake::setMode(PINES_FB[i], INPUT_PULLDOWN);
        fake::setPin(PINES_FB[i], LOW);          // contactor en reposo
        fake::setMode(PINES_PLC[i], INPUT_PULLUP);
        fake::setPin(PINES_PLC[i], LOW);         // el PLC no ordena
    }
    fake::setPin(PIN_MODO_GEN, pin_modo_generador ? LOW : HIGH);

    rl.begin();
    sm.begin();
    // Siembra el feedback inicial que hace main.cpp:263-265
    for (int i = 0; i < NUM_BOMBAS; i++) sm.onFeedback(i, false);
}

static void arranque(StateMachine& sm, bool pin_modo_generador) {
    RelayControl rl_dummy;
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    for (int i = 0; i < NUM_BOMBAS; i++) {
        fake::setMode(PINES_FB[i], INPUT_PULLDOWN);
        fake::setPin(PINES_FB[i], LOW);
        fake::setMode(PINES_PLC[i], INPUT_PULLUP);
        fake::setPin(PINES_PLC[i], LOW);
    }
    fake::setPin(PIN_MODO_GEN, pin_modo_generador ? LOW : HIGH);
    sm.begin();
    for (int i = 0; i < NUM_BOMBAS; i++) sm.onFeedback(i, false);
}

/**
 * Provoca un fault de feedback REAL (SIF-02): el PLC ordena arrancar la
 * bomba, el contactor no confirma el retorno, y pasados 2 s el supervisor
 * incrementa fault_count y lacha la bomba. Todo por la API pública.
 */
static void faultDeFeedback(StateMachine& sm, uint8_t id) {
    sm.onPlcOrder(id, true);
    // 2.6 s a 100 Hz > FEEDBACK_TIMEOUT_MS (2000 ms)
    for (int i = 0; i < 260; i++) tick(sm);
}

/**
 * Reproduce un comando remoto MODO_CHANGE tal y como lo ejecuta
 * CommBridge::handleCommand() (comm_bridge.cpp:139-143):
 *     case McuEvent::MODO_CHANGE: sm.setModoGenerador(msg.payload & 0x01);
 * Un solo payload bit0=0 llama a setModoGenerador(false).
 */
static void comandoRemotoModo(StateMachine& sm, bool generador) {
    sm.setModoGenerador(generador);
}

// ============================================================
// PARTE 3 / matriz de puedeArrancar(): 4 estados x 3 bombas
// ============================================================

static void test_puede_arrancar_matriz_4_estados_x_3_bombas() {
    nuevoTest();
    // --- NORMAL: las 3 bombas permitidas ---
    {
        StateMachine sm;
        arranque(sm, false);
        ticks(sm, 5);
        CHECK(sm.getState() == SystemState::NORMAL);
        for (uint8_t b = 0; b < NUM_BOMBAS; b++) {
            CHECK(sm.puedeArrancar(b) == true);
        }
    }
    // --- GENERADOR: sólo la bomba 0 ---
    {
        StateMachine sm;
        arranque(sm, true);
        ticks(sm, 5);
        CHECK(sm.getState() == SystemState::GENERADOR);
        for (uint8_t b = 0; b < NUM_BOMBAS; b++) {
            CHECK(sm.puedeArrancar(b) == (b == 0));
        }
    }
    // --- EMERGENCIA: ninguna ---
    {
        StateMachine sm;
        arranque(sm, false);
        ticks(sm, 5);
        sm.triggerEmergencia(ERR_SENSOR_CORRIENTE);
        CHECK(sm.getState() == SystemState::EMERGENCIA);
        for (uint8_t b = 0; b < NUM_BOMBAS; b++) {
            CHECK(sm.puedeArrancar(b) == false);
        }
    }
    // --- MANTENIMIENTO: ninguna ---
    {
        StateMachine sm;
        arranque(sm, false);
        ticks(sm, 5);
        sm.setMantenimiento(true);
        CHECK(sm.getState() == SystemState::MANTENIMIENTO);
        for (uint8_t b = 0; b < NUM_BOMBAS; b++) {
            CHECK(sm.puedeArrancar(b) == false);
        }
    }
}

static void test_puede_arrancar_con_fault_en_los_4_estados() {
    nuevoTest();
    // Mismo recorrido, pero con una bomba con fault_count > 0.
    struct Escenario { const char* nombre; bool pin_gen; bool emergencia; bool mant; };
    const Escenario casos[] = {
        {"NORMAL",       false, false, false},
        {"GENERADOR",    true,  false, false},
        {"EMERGENCIA",   false, true,  false},
        {"MANTENIMIENTO", false, false, true },
    };
    for (const Escenario& c : casos) {
        StateMachine sm;
        arranque(sm, c.pin_gen);
        ticks(sm, 5);
        // Provocar el fault hay que hacerlo en NORMAL/GENERADOR (en
        // emergencia el PLC no llega a cerrar el relé).
        if (!c.emergencia && !c.mant) faultDeFeedback(sm, 2);   // B3
        if (c.emergencia)  sm.triggerEmergencia(ERR_SENSOR_CORRIENTE);
        if (c.mant)        sm.setMantenimiento(true);

        for (uint8_t b = 0; b < NUM_BOMBAS; b++) {
            // El fault es de B3 (id 2) y la bloquea a ella sola. Las sanas
            // (0 y 1) siguen la regla del modo; un fault es un estado de
            // la bomba, no una emergencia global.
            bool permitido_por_modo;
            if (c.emergencia || c.mant) {
                permitido_por_modo = false;
            } else if (c.pin_gen) {
                permitido_por_modo = (b == 0);
            } else {
                permitido_por_modo = true;
            }
            bool esperado = permitido_por_modo && (b != 2);
            CHECK(sm.puedeArrancar(b) == esperado);
        }
        // El pin de B3 nunca se energiza
        if (!c.emergencia && !c.mant) CHECK(!sm.getRelayClosed(2));
        (void)c.nombre;
    }
}

static void test_puede_arrancar_rechaza_ids_fuera_de_rango() {
    nuevoTest();
    StateMachine sm;
    arranque(sm, false);
    ticks(sm, 5);
    // Un id >= NUM_BOMHAS no debe indexar fuera del array
    CHECK(sm.puedeArrancar(NUM_BOMBAS) == false);
    CHECK(sm.puedeArrancar(NUM_BOMBAS + 1) == false);
    CHECK(sm.puedeArrancar(255) == false);
    CHECK(sm.getRelayClosed(NUM_BOMBAS) == false);
    CHECK(sm.getRelayClosed(255) == false);
    // onPlcOrder / onFeedback también tienen que descartar ids inválidos
    sm.onPlcOrder(200, true);
    sm.onFeedback(200, true);
    ticks(sm, 10);
    CHECK(bombsCerradas(sm) == 0);
}

// ============================================================
// INVARIANTE 1 - el límite de 1 bomba en GENERADOR (SIF-01)
// ============================================================

static void test_boot_en_generador_nace_bloqueado() {
    nuevoTest();
    // Antes incluso del primer update(): con el pin en LOW la máquina nace
    // ya en GENERADOR (state_machine.cpp:45-49).
    StateMachine sm;
    arranque(sm, true);
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(sm.puedeArrancar(0) == true);
    CHECK(sm.puedeArrancar(1) == false);
    CHECK(sm.puedeArrancar(2) == false);
}

static void test_generador_mantiene_bloqueo_con_ordenes_plc_persistentes() {
    nuevoTest();
    // El PLC pide las 3 bombas y las mantiene pedidas (contacto seco
    // cerrado). En GENERADOR sólo B1 puede arrancar, para siempre.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);
    int max_cerradas = 0;
    for (int i = 0; i < 300; i++) {
        sm.onPlcOrder(0, true);
        sm.onPlcOrder(1, true);
        sm.onPlcOrder(2, true);
        tick(sm, rl);
        int c = bombsCerradas(sm);
        if (c > max_cerradas) max_cerradas = c;
    }
    CHECK(max_cerradas <= 1);
    CHECK(sm.getRelayClosed(1) == false);
    CHECK(sm.getRelayClosed(2) == false);
    CHECK(fake::getPin(PIN_RELE_BOMBA2) == RELAY_INACTIVE_LEVEL);
    CHECK(fake::getPin(PIN_RELE_BOMBA3) == RELAY_INACTIVE_LEVEL);
    // Y con la orden puesta, sigue sin arrancar
    CHECK(sm.puedeArrancar(1) == false);
}

static void test_borde_de_cambio_de_estado_red_a_generador() {
    nuevoTest();
    // Las 3 bombas corriendo en RED. El contacto del ATS se abre y el pin
    // pasa a LOW (GENERADOR). A partir del cambio, B2/B3 no pueden volver
    // a cerrarse ni un solo ciclo.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, false);
    for (uint8_t b = 0; b < NUM_BOMBAS; b++) sm.onPlcOrder(b, true);
    ticks(sm, rl, 30);
    CHECK(bombsCerradas(sm) == 3);      // en RED sí, las 3
    CHECK(fake::getPin(PIN_RELE_BOMBA2) == RELAY_ACTIVE_LEVEL);

    // El ATS conmuta a GENERADOR
    fake::setPin(PIN_MODO_GEN, LOW);

    bool violate = false;
    for (int i = 0; i < 100; i++) {
        tick(sm, rl);
        if (sm.getState() == SystemState::GENERADOR &&
            (sm.getRelayClosed(1) || sm.getRelayClosed(2))) {
            violate = true;
        }
    }
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(!violate);
    CHECK(fake::getPin(PIN_RELE_BOMBA2) == RELAY_INACTIVE_LEVEL);
    CHECK(fake::getPin(PIN_RELE_BOMBA3) == RELAY_INACTIVE_LEVEL);
}

static void test_ventana_entre_read_modo_y_update_bombas() {
    nuevoTest();
    // Invariante intra-ciclo: en el momento en que updateBombas() decide,
    // current_state ya debe reflejar el pin de modo. Si algún camino
    // llegara a updateBombas() con el flag de modo ya actualizado pero el
    // estado aún en NORMAL, esta comprobación lo cazaría.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);
    for (uint8_t b = 0; b < NUM_BOMBAS; b++) sm.onPlcOrder(b, true);
    bool violate = false;
    for (int i = 0; i < 200; i++) {
        tick(sm, rl);
        if (sm.getState() == SystemState::GENERADOR) {
            if (sm.puedeArrancar(1) || sm.puedeArrancar(2)) violate = true;
            if (sm.getRelayClosed(1) || sm.getRelayClosed(2)) violate = true;
        }
        // Y el estado nunca puede ser NORMAL con el pin en LOW durante más
        // que el debounce (5 lecturas = 50 ms a 100 Hz).
        if (fake::getPin(PIN_MODO_GEN) == LOW && i > 20 &&
            sm.getState() == SystemState::NORMAL) {
            violate = true;
        }
    }
    CHECK(!violate);
}

static void test_reset_emergencia_no_abre_b2_en_generador() {
    nuevoTest();
    // operatorReset() llama a transitionTo(computeDesiredState()), que en
    // GENERADOR devuelve GENERADOR. Debe respectarlo.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);
    sm.onPlcOrder(1, true);
    sm.onPlcOrder(2, true);
    sm.triggerEmergencia(ERR_SOBRECARGA);
    ticks(sm, rl, 10);
    CHECK(sm.getState() == SystemState::EMERGENCIA);
    sm.resetEmergencia();
    ticks(sm, rl, 30);
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(!sm.getRelayClosed(1));
    CHECK(!sm.getRelayClosed(2));
    CHECK(fake::getPin(PIN_RELE_BOMBA2) == RELAY_INACTIVE_LEVEL);
}

static void test_set_mantenimiento_no_abre_b2_en_generador() {
    nuevoTest();
    // setMantenimiento(false) hace transitionTo(modo_generador_hw ? GEN :
    // NORMAL) (state_machine.cpp:207). Con el pin en LOW debe volver a GEN.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);
    sm.onPlcOrder(1, true);
    sm.onPlcOrder(2, true);
    sm.setMantenimiento(true);
    ticks(sm, rl, 10);
    CHECK(sm.getState() == SystemState::MANTENIMIENTO);
    sm.setMantenimiento(false);
    ticks(sm, rl, 30);
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(!sm.getRelayClosed(1));
    CHECK(!sm.getRelayClosed(2));
    CHECK(fake::getPin(PIN_RELE_BOMBA2) == RELAY_INACTIVE_LEVEL);
}

static void test_auto_recover_no_abre_b2_en_generador() {
    nuevoTest();
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);
    sm.onPlcOrder(1, true);
    sm.onPlcOrder(2, true);
    sm.triggerEmergencia(ERR_SOBRECARGA);
    ticks(sm, rl, 10);
    CHECK(sm.autoRecoverTrip() == true);
    ticks(sm, rl, 30);
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(!sm.getRelayClosed(1));
    CHECK(!sm.getRelayClosed(2));
    CHECK(fake::getPin(PIN_RELE_BOMBA2) == RELAY_INACTIVE_LEVEL);
}

// ---- ATAQUE A: un único comando remoto MODO_CHANGE(RED) ----
//
// docs/security_protocols.md:94-97 dice: "Existe comando
// set_modo_generador (solo admin, 2FA) para forzar modo HW en casos
// excepcionales, PERO EL FIRMWARE SIGUE BLOQUEANDO B2/B3 SI EL PIN HW DICE
// GEN." Este test comprueba exactamente eso.
static void test_comando_remoto_modo_una_sola_vez() {
    nuevoTest();
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);
    CHECK(sm.getState() == SystemState::GENERADOR);
    // El PLC mantiene B2 y B3 pedidos desde antes del comando
    sm.onPlcOrder(1, true);
    sm.onPlcOrder(2, true);
    ticks(sm, rl, 20);
    CHECK(!sm.getRelayClosed(1));
    CHECK(!sm.getRelayClosed(2));

    // El pin HW sigue en LOW (GENERADOR). Llega un MODO_CHANGE con bit0=0.
    comandoRemotoModo(sm, false);

    int max_cerradas = 0;
    bool b2_o_b3_cerrada = false;
    bool pin_b2_energizado = false;
    for (int i = 0; i < 30; i++) {   // 300 ms
        tick(sm, rl);
        int c = bombsCerradas(sm);
        if (c > max_cerradas) max_cerradas = c;
        if (sm.getRelayClosed(1) || sm.getRelayClosed(2)) b2_o_b3_cerrada = true;
        if (fake::getPin(PIN_RELE_BOMBA2) == RELAY_ACTIVE_LEVEL) pin_b2_energizado = true;
    }
    // La invariante: con el pin de modo en LOW, B2/B3 nunca.
    CHECK(!b2_o_b3_cerrada);
    CHECK(!pin_b2_energizado);
    CHECK(max_cerradas <= 1);
    // Y al estabilizarse el sistema vuelve a GENERADOR
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(sm.puedeArrancar(1) == false);
}

// ---- ATAQUE B: comandos remotos repetidos manipulan el debounce ----
//
// setModoGenerador() pone modo_debounce_cnt = 0 (state_machine.cpp:153).
// readModoHardware() sólo restaura el modo del pin tras 5 lecturas
// consistentes (state_machine.cpp:337). Basta con reenviar el comando cada
// 50 ms para que el contador NUNCA llegue a 5 y el sistema se quede
// clavado en NORMAL con el pin en LOW.
static void test_comando_remoto_modo_repetido_manipula_el_debounce() {
    nuevoTest();
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);
    sm.onPlcOrder(1, true);
    sm.onPlcOrder(2, true);
    ticks(sm, rl, 20);
    CHECK(sm.getState() == SystemState::GENERADOR);

    int max_cerradas = 0;
    int ticks_con_b2_o_b3 = 0;
    int ticks_con_pin_b2 = 0;
    for (int i = 0; i < 300; i++) {   // 3 s
        tick(sm, rl);
        // 5 comandos/s por el puerto serie: 5*11*10 = 550 bit/s a 115200.
        if (i % 5 == 0) comandoRemotoModo(sm, false);
        int c = bombsCerradas(sm);
        if (c > max_cerradas) max_cerradas = c;
        if (sm.getRelayClosed(1) || sm.getRelayClosed(2)) ticks_con_b2_o_b3++;
        if (fake::getPin(PIN_RELE_BOMBA2) == RELAY_ACTIVE_LEVEL) ticks_con_pin_b2++;
    }
    CHECK(ticks_con_b2_o_b3 == 0);
    CHECK(ticks_con_pin_b2 == 0);
    CHECK(max_cerradas <= 1);
    // Ni siquiera al final vuelve a GENERADOR
    CHECK(sm.getState() == SystemState::GENERADOR);
}

static void test_comando_remoto_modo_no_puede_sacarse_de_generador_a_red() {
    nuevoTest();
    // El caso inverso: el pin dice LOW (GENERADOR) y el comando pone el flag
    // en RED. En GENERADOR el sistema tiene que estar en GENERADOR aunque
    // su propio flag diga RED. Se comprueba el estado resultante tras el
    // debounce normal (sin reenviar el comando).
    StateMachine sm;
    arranque(sm, true);
    comandoRemotoModo(sm, false);
    ticks(sm, 20);   // 200 ms > 50 ms de debounce
    CHECK(sm.getState() == SystemState::GENERADOR);
}

static void test_set_modo_generador_se_niega_con_emergencia() {
    nuevoTest();
    StateMachine sm;
    arranque(sm, true);
    sm.triggerEmergencia(ERR_SOBRECARGA);
    // Con emergencia activa setModoGenerador() devuelve false sin tocar nada
    CHECK(sm.setModoGenerador(false) == false);
    sm.resetEmergencia();
    ticks(sm, 5);
    CHECK(sm.getState() == SystemState::GENERADOR);

    // Un comando NO puede declarar RED mientras el pin del ATS diga
    // GENERADOR. Antes esta aserción era == true y codificaba
    // exactamente el bypass quelaubía arrancar B2/B3 con el cable en
    // GENERADOR. La regla acordada es que un comando solo puede
    // endurecer, nunca relajar por debajo del hardware.
    CHECK(sm.setModoGenerador(false) == false);
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(!sm.puedeArrancar(1));
    CHECK(!sm.puedeArrancar(2));
}

// ============================================================
// INVARIANTE 2 - nadie auto-recupera un fault de feedback
// ============================================================

static void test_control_el_fault_de_feedback_produce_bloqueo() {
    nuevoTest();
    StateMachine sm;
    arranque(sm, false);          // NORMAL
    ticks(sm, 5);
    faultDeFeedback(sm, 0);       // B1 pierde el retorno
    // La orden del PLC sigue puesta: el latch es lo único que la bloquea
    CHECK(sm.getRelayClosed(0) == false);
    CHECK(sm.puedeArrancar(0) == false);
    CHECK(fake::getPin(PIN_RELE_BOMBA1) == RELAY_INACTIVE_LEVEL);
}

static void test_auto_recover_no_limpia_faults() {
    nuevoTest();
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, false);      // NORMAL
    ticks(sm, rl, 5);
    faultDeFeedback(sm, 0);       // B1 con fault_count = 1
    CHECK(!sm.puedeArrancar(0));

    // Trip de sobrecarga del generador -> auto-recuperación
    sm.triggerEmergencia(ERR_SOBRECARGA);
    CHECK(sm.getState() == SystemState::EMERGENCIA);
    CHECK(sm.autoRecoverTrip() == true);
    CHECK(!sm.hayEmergencia());

    // LA INVARIANTE: el fault de feedback sigue ahí y la bomba sigue
    // bloqueada, sin intervención de nadie.
    CHECK(sm.puedeArrancar(0) == false);
    ticks(sm, rl, 100);           // 1 s: el relé no se mueve
    CHECK(!sm.getRelayClosed(0));
    CHECK(fake::getPin(PIN_RELE_BOMBA1) == RELAY_INACTIVE_LEVEL);
    // Y tampoco se "deslatcha" por sí sola con el paso del tiempo
    ticks(sm, rl, 500);
    CHECK(!sm.puedeArrancar(0));
    CHECK(!sm.getRelayClosed(0));
}

static void test_auto_recover_preserva_faults_tambien_en_generador() {
    nuevoTest();
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);       // GENERADOR (sólo B1 permitida)
    ticks(sm, rl, 5);
    faultDeFeedback(sm, 0);
    CHECK(!sm.puedeArrancar(0));

    sm.triggerEmergencia(ERR_SOBRECARGA);
    CHECK(sm.autoRecoverTrip() == true);
    ticks(sm, rl, 100);

    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(sm.puedeArrancar(0) == false);
    CHECK(!sm.getRelayClosed(0));
    CHECK(fake::getPin(PIN_RELE_BOMBA1) == RELAY_INACTIVE_LEVEL);
}

static void test_auto_recover_solo_para_sobrecarga() {
    nuevoTest();
    // Cualquier otro motivo de emergencia necesita a una persona.
    const uint16_t otros[] = {
        ERR_SENSOR_CORRIENTE,   // 0x6003
        ERR_CONTACTOR_PEGADO,   // 0x6002
        ERR_AVISO_SOBRECARGA,   // 0x6010
        ERR_TRIP_AUTO_RECOVER,  // 0x6020
        ERR_TRIP_LATCH,         // 0x6021
        0x0000, 0x0001, 0x1234, 0xFFFF,
    };
    for (uint16_t codigo : otros) {
        StateMachine sm;
        arranque(sm, false);
        ticks(sm, 5);
        sm.triggerEmergencia(codigo);
        CHECK(sm.autoRecoverTrip() == false);
        CHECK(sm.getState() == SystemState::EMERGENCIA);
        CHECK(sm.hayEmergencia());
    }
}

static void test_auto_recover_sin_emergencia_no_hace_nada() {
    nuevoTest();
    StateMachine sm;
    arranque(sm, false);
    ticks(sm, 5);
    CHECK(sm.autoRecoverTrip() == false);
    CHECK(sm.getState() == SystemState::NORMAL);
    // Y no puede "re-armar" un protector que no ha disparado
    for (int i = 0; i < 50; i++) CHECK(sm.autoRecoverTrip() == false);
}

static void test_reset_emergencia_si_limpia_faults() {
    nuevoTest();
    // Contraprueba: el reset del OPERADOR sí tiene que liberar la bomba.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, false);
    ticks(sm, rl, 5);
    faultDeFeedback(sm, 0);
    CHECK(!sm.puedeArrancar(0));

    sm.resetEmergencia();
    CHECK(sm.puedeArrancar(0) == true);
    ticks(sm, rl, 30);
    CHECK(sm.getRelayClosed(0));
    CHECK(fake::getPin(PIN_RELE_BOMBA1) == RELAY_ACTIVE_LEVEL);
}

static void test_reset_emergencia_sin_emergencia_limpia_faults() {
    nuevoTest();
    // resetEmergencia() llama a clearFaults() también sin emergencia
    // (state_machine.cpp:165-169). Es un comando de operador, así que no es
    // una vía de auto-recuperación; se deja constancia explícita.
    StateMachine sm;
    arranque(sm, false);
    ticks(sm, 5);
    faultDeFeedback(sm, 0);
    CHECK(!sm.puedeArrancar(0));
    sm.resetEmergencia();
    CHECK(sm.puedeArrancar(0) == true);
}

static void test_transicion_de_estado_no_limpia_faults() {
    nuevoTest();
    // transitionTo() ya no borra faults (state_machine.cpp:211-225).
    // Ida y vuelta por MANTENIMIENTO y por EMERGENCIA no deben borrarlos.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, false);
    ticks(sm, rl, 5);
    faultDeFeedback(sm, 0);

    sm.setMantenimiento(true);
    ticks(sm, rl, 20);
    CHECK(sm.getState() == SystemState::MANTENIMIENTO);
    CHECK(!sm.puedeArrancar(0));

    sm.setMantenimiento(false);
    ticks(sm, rl, 20);
    CHECK(sm.getState() == SystemState::NORMAL);
    CHECK(!sm.puedeArrancar(0));   // <- el fault sigue

    sm.triggerEmergencia(ERR_SENSOR_CORRIENTE);
    ticks(sm, rl, 20);
    CHECK(!sm.puedeArrancar(0));

    // Salir de emergencia por la vía que NO limpia faults: el auto-rearm
    sm.resetEmergencia();          // operador: sí limpia
    CHECK(sm.puedeArrancar(0) == true);
}

static void test_codigo_de_emergencia_desde_el_cable_no_puede_falsificar_sobrecarga() {
    nuevoTest();
    // handleCommand() con McuEvent::STATE_CHANGE hace
    //     sm.triggerEmergencia(msg.payload >> 8)
    // (comm_bridge.cpp:149). El payload es uint16, así que el código de
    // error recibido por el cable sólo puede valer 0x0000..0x00FF:
    // ERR_SOBRECARGA = 0x6001 es inalcanzable. Por eso la puerta de
    // autoRecoverTrip() NO se puede forzar inyectando un comando remoto.
    // Se comprueba el truncamiento con TODOS los payloads de 16 bits que
    // el parser puede dejar pasar.
    bool alguien_pudo_falsificar = false;
    for (uint32_t p = 0; p <= 0xFFFFu; p++) {
        StateMachine sm;
        arranque(sm, false);
        ticks(sm, 5);
        uint16_t payload = static_cast<uint16_t>(p);
        if ((payload & 0xFF) == static_cast<uint8_t>(SystemState::EMERGENCIA)) {
            sm.triggerEmergencia(static_cast<uint16_t>(payload >> 8));
            if (sm.autoRecoverTrip()) alguien_pudo_falsificar = true;
        }
    }
    CHECK(!alguien_pudo_falsificar);
    // Y el valor máximo alcanzable por el cable es 0x00FF
    StateMachine sm;
    arranque(sm, false);
    ticks(sm, 5);
    sm.triggerEmergencia(static_cast<uint16_t>(0xFFFFu >> 8));
    CHECK(sm.autoRecoverTrip() == false);
}

// ============================================================
// INVARIANTE 7 - el cable cortado da el estado restrictivo
// ============================================================

static void test_cable_del_modo_cortado_es_fail_open() {
    nuevoTest();
    // Una version anterior de los docs afirmaba "cable cortado del modo ->
    // GENERADOR restrictivo, es el estado seguro correcto". Era falso y ya
    // se corrigio en hardware_spec.md. Este test fija el comportamiento
    // REAL para que el fail-open quede escrito y visible.
    //
    // Lo que hace el cableado declarado: pull-up (hardware_spec.md:108 y
    // mode_detect.cpp:16 pinMode(PIN_MODO_GEN, INPUT_PULLUP)). Con pull-up,
    // un cable cortado deja el pin flotante pulled-up -> HIGH. Y el
    // firmware lee HIGH como RED (state_machine.cpp:40 y :330).
    StateMachine sm;
    RelayControl rl;

    // Estado eléctrico real de un cable de modo cortado con pull-up
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    fake::cortarCable(PIN_MODO_GEN);
    for (int i = 0; i < NUM_BOMBAS; i++) {
        fake::setMode(PINES_RELE[i], OUTPUT);
        fake::setPin(PINES_RELE[i], RELAY_INACTIVE_LEVEL);
        fake::setMode(PINES_PLC[i], INPUT_PULLUP);
        fake::setPin(PINES_PLC[i], HIGH);   // el PLC pide las 3
    }
    rl.begin();
    sm.begin();
    for (int i = 0; i < NUM_BOMBAS; i++) sm.onFeedback(i, true);
    for (int i = 0; i < NUM_BOMBAS; i++) sm.onPlcOrder(i, true);
    ticks(sm, rl, 100);

    // COMPORTAMIENTO REAL, no el que se creia documentado.
    //
    // El pin del ATS va con INPUT_PULLUP, asi que un cable cortado lo deja
    // flotando a HIGH. HIGH = RED, o sea el modo PERMISIVO: las 3 bombas
    // pueden arrancar. Una version anterior de docs/hardware_spec.md
    // afirmaba lo contrario ("cable cortado -> GENERADOR, restrictivo") y
    // era falsa.
    //
    // Esto NO es un bug de software: un pin flotante a HIGH es un nivel
    // electrico valido y el firmware no tiene forma de distinguirlo de una
    // RED legitima. La unica correccion es de hardware:
    //   1. pull-up 10k EXTERNO en PCB (no solo el interno del MCU)
    //   2. verificar el contacto del ATS en PT-02
    // Registrado como N-02 en docs/hazop_lopa.md (L2xS4 = Alto).
    //
    // Este test existe para que el fail-open este escrito y sea visible,
    // no para fingir que el codigo lo neutralize.
    CHECK(fake::getMode(PIN_MODO_GEN) == INPUT_PULLUP);
    CHECK(sm.getState() == SystemState::NORMAL);   // HIGH = RED
    CHECK(sm.puedeArrancar(1));                   // permisivo: el hallazgo
    CHECK(sm.puedeArrancar(2));
}

static void test_cable_de_feedback_cortado_deja_la_bomba_parada() {
    nuevoTest();
    // docs/hardware_spec.md:109 -> "Feedback B1 | PC1 | pull-down | LOW ->
    // bomba parada". Con pull-down, cable cortado = LOW = sin retorno
    // aux. El PLC ordena arrancar: SIF-02 debe abrir el relé y lachar la
    // bomba tras FEEDBACK_TIMEOUT_MS.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, false);       // RED
    for (int i = 0; i < NUM_BOMBAS; i++) {
        fake::setMode(PINES_FB[i], INPUT_PULLDOWN);
        fake::cortarCable(PINES_FB[i]);       // LOW
    }
    ticks(sm, rl, 5);

    // B1: el PLC la pide, el retorno no llega
    sm.onPlcOrder(0, true);
    sm.onFeedback(0, false);
    // B2: el PLC no la pide, tampoco hay retorno -> sin fallo (bien)
    sm.onPlcOrder(1, false);
    sm.onFeedback(1, false);

    ticks(sm, rl, 260);            // 2.6 s > 2 s de timeout

    CHECK(!sm.puedeArrancar(0));   // lachada por pérdida de feedback
    CHECK(!sm.getRelayClosed(0));
    CHECK(fake::getPin(PIN_RELE_BOMBA1) == RELAY_INACTIVE_LEVEL);
    // Y una bomba sana que no ha perdido el retorno sigueBlocked
    CHECK(sm.puedeArrancar(2) == true);
    // Sin embargo: el relé estuvo cerrado los primeros 2 s.
    // (Es el comportamiento documentado de SIF-02, no lo contamos como
    // fallo, pero se deja el valor medido.)
}

static void test_contactor_pegado_bloquea_la_bomba() {
    nuevoTest();
    // SIF-03 (docs/security_protocols.md:30): PLC=OFF, FB=ON > 2s ->
    // "EMERGENCIA global".
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, false);
    ticks(sm, rl, 5);
    // El contactor está pegado: el retorno aux marca HIGH sin orden
    fake::setPin(PIN_FEEDBACK_B1, HIGH);
    sm.onFeedback(0, true);
    sm.onPlcOrder(0, false);
    ticks(sm, rl, 260);

    // Lo mínimo: la bomba queda bloqueada (esto sí ocurre)
    CHECK(!sm.puedeArrancar(0));
    CHECK(!sm.getRelayClosed(0));

    // Lo que la doc promete: emergencia GLOBAL
    CHECK(sm.getState() == SystemState::EMERGENCIA);
    // y por tanto las otras dos bombas también deberían estar paradas
    CHECK(!sm.puedeArrancar(1));
    CHECK(!sm.puedeArrancar(2));
}

static void test_modo_generador_no_dispara_faults() {
    nuevoTest();
    // En GENERADOR las bombas 2 y 3 están bloqueadas por decisión nuestra:
    // que no haya feedback NO es un fallo (state_machine.cpp:300-301).
    // Un falso fault aquí podría lachar bombas que no tienen nada malo.
    StateMachine sm;
    RelayControl rl;
    arranque(sm, rl, true);
    for (int i = 0; i < NUM_BOMBAS; i++) {
        sm.onPlcOrder(i, true);
        sm.onFeedback(i, false);
    }
    ticks(sm, rl, 500);
    // B2/B3 nunca cerraron -> nunca hubo mismatch -> nada que lachar
    CHECK(!sm.getRelayClosed(1));
    CHECK(!sm.getRelayClosed(2));
    // Bloqueadas por el MODO, no por un fault
    CHECK(sm.puedeArrancar(1) == false);
    CHECK(sm.puedeArrancar(2) == false);

    // La prueba de que no acumularon fault: al volver a RED deben quedar
    // disponibles de inmediato, sin reset de operador.
    fake::setPin(PIN_MODO_GEN, HIGH);          // el ATS vuelve a RED
    ticks(sm, rl, 100);
    CHECK(sm.getState() == SystemState::NORMAL);
    CHECK(sm.puedeArrancar(2) == true);         // B3 sana, sin latching
    CHECK(sm.puedeArrancar(1) == true);         // B2 sana, sin latching

    // B1 sí cerró su relé (en GENERADOR puede) y sin feedback durante 5 s
    // -> SIF-02 le válida un fault LEGÍTIMO: el PLC la pidió y el
    // contactor no confirmó. No es un falso positivo.
    CHECK(sm.puedeArrancar(0) == false);
}

// ============================================================
// Robustez del orden de arranque (SIF-01 depende de él)
// ============================================================

static void test_state_machine_configura_su_propio_pin() {
    nuevoTest();
    // state_machine.cpp lee PIN_MODO_GEN en begin() (línea 40) y en
    // readModoHardware() sin haber llamado nunca a pinMode() sobre él.
    // La entrada depende de que modeDetect.begin() se haya ejecutado antes.
    // Si alguien reordena setup(), o usa StateMachine fuera de main.cpp,
    // la entrada queda flotante y el modo es indeterminado.
    StateMachine sm;
    nuevoTest();
    sm.begin();   // sin configurar ningún pin antes
    CHECK(fake::getMode(PIN_MODO_GEN) == INPUT_PULLUP);
}

// ============================================================

int main() {
    std::printf("--- Maquina de estados (SIF-01 / SIF-02 / fail-safe de cable) ---\n");

    std::printf("  [matriz puedeArrancar]\n");
    test_puede_arrancar_matriz_4_estados_x_3_bombas();
    test_puede_arrancar_con_fault_en_los_4_estados();
    test_puede_arrancar_rechaza_ids_fuera_de_rango();

    std::printf("  [invariante 1: limite de 1 bomba en GENERADOR]\n");
    test_boot_en_generador_nace_bloqueado();
    test_generador_mantiene_bloqueo_con_ordenes_plc_persistentes();
    test_borde_de_cambio_de_estado_red_a_generador();
    test_ventana_entre_read_modo_y_update_bombas();
    test_reset_emergencia_no_abre_b2_en_generador();
    test_set_mantenimiento_no_abre_b2_en_generador();
    test_auto_recover_no_abre_b2_en_generador();
    test_comando_remoto_modo_una_sola_vez();
    test_comando_remoto_modo_repetido_manipula_el_debounce();
    test_comando_remoto_modo_no_puede_sacarse_de_generador_a_red();
    test_set_modo_generador_se_niega_con_emergencia();

    std::printf("  [invariante 2: nadie auto-recupera un fault]\n");
    test_control_el_fault_de_feedback_produce_bloqueo();
    test_auto_recover_no_limpia_faults();
    test_auto_recover_preserva_faults_tambien_en_generador();
    test_auto_recover_solo_para_sobrecarga();
    test_auto_recover_sin_emergencia_no_hace_nada();
    test_reset_emergencia_si_limpia_faults();
    test_reset_emergencia_sin_emergencia_limpia_faults();
    test_transicion_de_estado_no_limpia_faults();
    test_codigo_de_emergencia_desde_el_cable_no_puede_falsificar_sobrecarga();

    std::printf("  [invariante 7: cable cortado]\n");
    test_cable_del_modo_cortado_es_fail_open();
    test_cable_de_feedback_cortado_deja_la_bomba_parada();
    test_contactor_pegado_bloquea_la_bomba();
    test_modo_generador_no_dispara_faults();

    std::printf("  [robustez de arranque]\n");
    test_state_machine_configura_su_propio_pin();

    std::printf("%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (maquina de estados)\n");
        return 0;
    }
    return 1;
}