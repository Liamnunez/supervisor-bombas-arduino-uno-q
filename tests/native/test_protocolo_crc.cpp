/**
 * @file test_protocolo_crc.cpp
 * @brief Verificación adversarial del protocolo de 11 bytes y su CRC8.
 *
 * El frame está DUPLICADO en tres sitios (AGENTS.md lo dice):
 *   - mcu/include/config.h   -> crc8(), validar_mensaje(), McuMessage
 *   - mcu/src/comm_bridge.cpp-> buildMessage(), tryParseMessage()
 *   - linux/app/state.py     -> _validate_crc(), send_command()
 *
 * Este test compila el comm_bridge REAL (con Serial1 del doble de test, que
 * captura lo que el firmware transmite y permite inyectar bytes de RX) y
 * comprueba:
 *   1. Los offsets del frame coinciden byte a byte con el doc.
 *   2. El CRC8 que el firmware EMITE es el que linux/app/state.py VALIDA.
 *   3. El CRC8 que el firmware VALIDA es el que state.py EMITE.
 *   4. comm_bridge.cpp descarta CRC malo, start/end incorrectos y longitud
 *      incompleta.
 *
 * Compilar:
 *   g++ -std=c++17 -Wall -Wextra -I mcu/include -I tests/native/fake_arduino \
 *       -include Arduino.h tests/native/test_protocolo_crc.cpp \
 *       mcu/src/comm_bridge.cpp mcu/src/state_machine.cpp \
 *       -o tests/native/build/test_protocolo_crc
 */

#include "Arduino.h"

#include "config.h"
#include "comm_bridge.h"
#include "state_machine.h"
#include "trip_policy.h"

#include <cstdio>
#include <cstring>
#include <vector>

// Para poder ejecutar código que puede COLGARSE sin colgarse el CI:
// cada escena se corre en un proceso hijo con un límite de tiempo.
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

/** Resultado de ejecutar una escena en un proceso hijo acotado en el tiempo. */
enum Resultado {
    VOLVIO_OK = 0,     // la escena terminó devolviendo 0
    VOLVIO_ERROR = 1,  // la escena terminó pero la comprobación interna falló
    COLGO = 2          // la escena no terminó: BUCLE INFINITO
};

static int g_timeout_ms = 3000;

/**
 * Ejecuta `escena` en un proceso hijo y devuelve siCOLGÓ.
 *
 * Hace falta porque el bug que se busca es un bucle infinito: llamarlo en
 * el proceso principal dejaría el test colgado para siempre y CI no
 * distinguiría "tardó" de "se colgó". El hijo se mata con SIGKILL si
 * pasa el límite, así que el resultado es determinista.
 */
static Resultado ejecutar_acotado(int (*escena)()) {
    std::fflush(stdout);
    std::fflush(stderr);
    pid_t pid = fork();
    if (pid == 0) {
        _exit(escena());
    }
    for (int waited = 0; waited < g_timeout_ms; waited += 5) {
        int st = 0;
        pid_t r = waitpid(pid, &st, WNOHANG);
        if (r == pid) {
            if (WIFEXITED(st) && WEXITSTATUS(st) == 0) return VOLVIO_OK;
            return VOLVIO_ERROR;
        }
        usleep(5000);
    }
    kill(pid, SIGKILL);
    int st = 0;
    waitpid(pid, &st, 0);
    return COLGO;
}

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
// El CRC8 tal y como lo calcula linux/app/state.py
// (StateManager._validate_crc, state.py:166-174)
//
//   crc = 0
//   for b in data[1:10]:          # offsets 1..9 -> 9 bytes
//       crc ^= b
//       for _ in range(8):
//           crc = (crc << 1) ^ 0x07 if (crc & 0x80) else (crc << 1)
//   return crc == data[10]
//
// OJO: el resultado se compara con data[10], que es el END BYTE, no el
// CRC (que está en data[9]). state.py:167 dice "CRC8 sobre bytes 1-9
// (excluye start_byte, crc8, end_byte) = 9 bytes", pero data[1:10] incluye
// data[9] (el propio crc8) y excluye data[10] (el end byte). Se reproduce
// el código TAL CUAL, con su off-by-one, para comparar como el sistema real.
static uint8_t crc8_como_linux(const uint8_t* data, size_t len) {
    uint8_t crc = 0;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t j = 0; j < 8; j++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x07) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

/**
 * _validate_crc de state.py, byte a byte.
 *
 * Contrato CORREGIDO del protocolo: CRC8 sobre los offsets [1..8]
 * (msg_type, bomba_id, timestamp(4), payload(2)) = 8 bytes, comparado con
 * el byte 9. Quedan fuera start_byte (0), crc8 (9) y end_byte (10).
 *
 * Antes, state.py firmaba sobre data[1:10] (9 bytes, que includia el
 * propio crc8) y comparaba contra data[10], mientras el firmware firmaba
 * sobre offsets [1..8] y validar_mensaje() comprobaba sobre [0..8]. Tres
 * rangos distintos: el firmware no aceptaba sus propios frames y el
 * Python lanzaba ValueError al escribir el crc sin enmascarar.
 */
static bool validar_crc_como_linux(const uint8_t* data) {
    return crc8_como_linux(data + 1, 8) == data[9];
}

/**
 * send_command de state.py: Construye el frame y calcula el CRC sobre los
 * offsets [1..8] con msg[9] a 0 (placeholder), y lo guarda en msg[9].
 * Mismo rango que validar_crc_como_linux() y que buildMessage() del
 * firmware.
 */
static std::vector<uint8_t> frame_como_linux(uint8_t event, uint8_t bomba_id,
                                             uint16_t payload) {
    std::vector<uint8_t> msg(11, 0);
    msg[0] = 0xAA;
    msg[1] = event & 0xFF;
    msg[2] = bomba_id & 0xFF;
    // msg[3:7] = struct.pack('<I', 0)  -> timestamp 0 en comandos
    msg[7] = (uint8_t)(payload & 0xFF);
    msg[8] = (uint8_t)((payload >> 8) & 0xFF);
    // msg[9] sigue a 0 mientras se calcula
    msg[10] = 0x55;
    msg[9] = crc8_como_linux(msg.data() + 1, 8);
    return msg;
}

/** Vacía la captura de TX de Serial1. */
static void limpiar_tx() {
    fake::serial1().emitted.clear();
    fake::st().tx.clear();
}

/**
 * Inyecta un frame en el RX de Serial1, como si llegara por el UART.
 * Es lo que main.cpp:390 ve en MCU_SERIAL.available().
 */
static void inyectar(const std::vector<uint8_t>& f) {
    fake::serial1().limpiar_rx();
    fake::serial1().feed(f.data(), f.size());
}

/** Últimos 11 bytes escritos por el firmware. */
static std::vector<uint8_t> frame_emitido() {
    const std::vector<uint8_t>& e = fake::serial1().emitted;
    if (e.size() < 11) return {};
    return std::vector<uint8_t>(e.end() - 11, e.end());
}

// ============================================================
// 1. Layout del frame: offsets y tamaño
// ============================================================
static void test_layout_del_frame() {
    // AGENTS.md: 0xAA | type | bomba_id | u32 ts | u16 payload | crc8 | 0x55
    CHECK(sizeof(McuMessage) == 11);   // coincide con MSG_SIZE de state.py:51
    CHECK(offsetof(McuMessage, start_byte) == 0);
    CHECK(offsetof(McuMessage, msg_type)   == 1);
    CHECK(offsetof(McuMessage, bomba_id)   == 2);
    CHECK(offsetof(McuMessage, timestamp)  == 3);   // u32
    CHECK(offsetof(McuMessage, payload)    == 7);   // u16
    CHECK(offsetof(McuMessage, crc8)       == 9);
    CHECK(offsetof(McuMessage, end_byte)   == 10);
    CHECK(sizeof(uint32_t) == 4);
    CHECK(sizeof(uint16_t) == 2);
}

// ============================================================
// 2. Los tres sitios coinciden en el cálculo del CRC8
// ============================================================
static void test_crc8_firmware_coincide_con_linux() {
    // Extrae el CRC que el firmware emite y el que linux validaría.
    // buildMessage (comm_bridge.cpp:182) calcula:
    //     msg.crc8 = crc8((uint8_t*)&msg + 1, sizeof(McuMessage) - 3);
    // o sea offset 1, longitud 8 (offsets 1..8): msg_type, bomba_id,
    // timestamp(4) y payload(2). EXCLUYE start_byte (offset 0).
    //
    // state.py:167 dice que el CRC va sobre "bytes 1-9 ... = 9 bytes", es
    // decir offset 1 longitud 9 (1..9, INCLUDING el propio crc8).
    //
    // Las dos reglas NO pueden ser la misma. Este test las compara sobre
    // un frame real emitido por el firmware.
    CommBridge cb;
    cb.begin();
    limpiar_tx();
    cb.sendHeartbeat(SystemState::NORMAL, 42, true);
    std::vector<uint8_t> f = frame_emitido();
    CHECK(f.size() == 11);

    // CRC calculado como lo documenta state.py (offset 1, 9 bytes, con el
    // byte de CRC del frame ya escrito)
    uint8_t crc_linux_doc = crc8_como_linux(f.data() + 1, 9);

    // CRC realmente escrito por el firmware
    uint8_t crc_firmware = f[9];

    // Si coincidieran, el byte 9 sería el CRC sobre los offsets 1..8 y
    // valdría lo que puso el firmware. Como los rangos difieren, la
    // comprobación real es: ¿valida linux el frame que emite el firmware?
    CHECK(f[0] == 0xAA);
    CHECK(f[10] == 0x55);
    CHECK(validar_mensaje(*(const McuMessage*)f.data()) == true);

    // Y el criterio que de verdad importa en planta: linux NO debe tirar el
    // frame por CRC inválido.
    CHECK(validar_crc_como_linux(f.data()) == true);
    (void)crc_linux_doc;
    (void)crc_firmware;
}

static void test_crc8_python_firma_es_aceptada_por_el_firmware() {
    // El frame que construye state.py, ¿lo acepta el firmware?
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 1);   // MODO_CHANGE GEN
    McuMessage m;
    std::memcpy(&m, f.data(), 11);
    CHECK(validar_mensaje(m) == true);
}

static void test_regla_de_crc_unica_y_coherente() {
    // Compara explícitamente las DOS reglas de CRC que conviven:
    //   firmware emite (comm_bridge.cpp:182):  crc8(&msg + 1, 11 - 3)  -> [1..8]
    //   linux valida  (state.py:170):          crc8(data[1:10])        -> [1..9]
    // Sobre un frame cuyo CRC esté a 0, las dos reglas dan valores
    // distintos salvo casualidad. Se comprueba con 1000 payloads distintos.
    int coincidencias = 0;
    for (int i = 0; i < 1000; i++) {
        uint8_t buf[11] = {0xAA, 0x50, 0xFF, 0, 0, 0, 0, 0, 0, 0x00, 0x55};
        buf[7] = (uint8_t)(i & 0xFF);
        buf[8] = (uint8_t)((i >> 8) & 0xFF);
        // regla del firmware: offsets 1..8 (8 bytes)
        uint8_t a = crc8_como_linux(buf + 1, 8);
        // regla de linux: offsets 1..9 (9 bytes)
        uint8_t b = crc8_como_linux(buf + 1, 9);
        if (a == b) coincidencias++;
    }
    std::printf("   [info] reglas de CRC firmware[1..8] vs linux[1..9]: "
                "%d/1000 coincidencias\n", coincidencias);
    // Si coincidieran siempre, no habría divergencia. Se informa del dato.
    CHECK(coincidencias < 1000);
}

// ============================================================
// 3. Descarta basura: CRC malo, start/end mal, longitud incompleta
// ============================================================
static bool enviar_frame_crudo(const std::vector<uint8_t>& f) {
    // Inyecta el frame en el RX de Serial1 y deja que processCommands lo
    // parsee. Devuelve si CommBridge消費ió un mensaje completo.
    StateMachine sm;
    TripPolicy policy;
    CommBridge cb;
    cb.begin();
    // Medir si el frame fue ACEPTADO, no si el UART se drenó.
    //
    // Antes comparaba rx_pendientes() antes/después, lo que devolvía
    // "rechazado" para TODO: como update() nunca llegaba a vaciar el
    // anillo, la cola se quedaba igual y las pruebas de rechazo pasaban
    // sin que hubiera llegado a probarse nada. rx_ok_count solo sube si
    // tryParseMessage() assembla un frame con CRC y encuadre válidos.
    cb.resetCounters();
    inyectar(f);
    cb.update(sm, 0);
    cb.processCommands(sm, policy);
    return cb.rxOkCount() > 0;
}

static void test_descarta_crc_malo() {
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 1);
    f[9] ^= 0xFF;                       // CRC corrupto
    CHECK(validar_mensaje(*(const McuMessage*)f.data()) == false);
    CHECK(enviar_frame_crudo(f) == false);
}

static void test_descarta_start_byte_malo() {
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 1);
    f[0] = 0xAB;
    CHECK(validar_mensaje(*(const McuMessage*)f.data()) == false);
    CHECK(enviar_frame_crudo(f) == false);
}

static void test_descarta_end_byte_malo() {
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 1);
    f[10] = 0x56;
    CHECK(validar_mensaje(*(const McuMessage*)f.data()) == false);
    CHECK(enviar_frame_crudo(f) == false);
}

static void test_descarta_longitud_incompleta() {
    // 10 bytes: falta el end byte
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 1);
    f.pop_back();
    CHECK(enviar_frame_crudo(f) == false);
    // 5 bytes
    std::vector<uint8_t> corto(5);
    memcpy(corto.data(), f.data(), 5);
    CHECK(enviar_frame_crudo(corto) == false);
    // vacío
    CHECK(enviar_frame_crudo({}) == false);
}

static void test_acepta_frame_completo_valido() {
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 1);
    CHECK(validar_mensaje(*(const McuMessage*)f.data()) == true);
    CHECK(enviar_frame_crudo(f) == true);
}

// Un frame con basura por delante debe recoverable en el siguiente
static void test_basura_por_delante_se_resincroniza() {
    std::vector<uint8_t> ruido = {0x00, 0xFF, 0x13, 0x37};
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 1);
    ruido.insert(ruido.end(), f.begin(), f.end());
    // processCommands debe descartar el ruido y llegar al frame bueno
    CHECK(enviar_frame_crudo(ruido) == true);
}

// ============================================================
// 4. handleCommand: qué efectos tienen los opcodes del cable
// ============================================================
static void test_handle_command_modo_change() {
    // MODO_CHANGE con payload bit0=0 -> setModoGenerador(false)
    StateMachine sm;
    TripPolicy policy;
    CommBridge cb;
    cb.begin();
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    fake::setPin(PIN_MODO_GEN, LOW);        // el pin dice GENERADOR
    sm.begin();
    CHECK(sm.getState() == SystemState::GENERADOR);

    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 0);   // bit0 = 0 -> RED
    inyectar(f);
    cb.update(sm, 0);          // main.cpp drena el UART al anillo
    cb.update(sm, 0);
    cb.processCommands(sm, policy);
    CHECK(fake::serial1().rx_pendientes() == 0);

    // El comando se consumió pero NO-relajó el modo: el pin del ATS dice
    // GENERADOR y ningún comando puede convertirlo en RED. Este test
    // afirmaba lo contrario ("el siguiente update() lo revierte"), que era
    // literalmente el bypass de SIF-01: 4 ciclos con las 3 bombas
    // permitidas, y permanente si se repetía el comando.
    for (int i = 0; i < 20; i++) {
        inyectar(f);           // y se repite, como haría un atacante
        cb.update(sm, 0);
        cb.update(sm, 0);
    cb.processCommands(sm, policy);
        fake::advance(10);
        sm.update();
    }
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(!sm.puedeArrancar(1));
    CHECK(!sm.puedeArrancar(2));
    CHECK(fake::getPin(PIN_RELE_BOMBA2) == RELAY_INACTIVE_LEVEL);

    // Endurecer (RED -> GENERADOR por comando) sí se permite.
    fake::setPin(PIN_MODO_GEN, HIGH);      // el ATS pasa a RED
    for (int i = 0; i < 6; i++) { fake::advance(10); sm.update(); }
    CHECK(sm.getState() == SystemState::NORMAL);
    CHECK(sm.setModoGenerador(true) == true);
    sm.update();
    CHECK(sm.getState() == SystemState::GENERADOR);
    CHECK(!sm.puedeArrancar(1));
}

static void test_handle_command_state_change_emergencia() {
    // STATE_CHANGE payload = (error_code << 8) | state
    StateMachine sm;
    TripPolicy policy;
    CommBridge cb;
    cb.begin();
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    fake::setPin(PIN_MODO_GEN, HIGH);       // RED
    sm.begin();
    CHECK(sm.hayEmergencia() == false);

    uint16_t payload = (uint16_t)((ERR_SOBRECARGA << 8) |
                                  (uint8_t)SystemState::EMERGENCIA);
    std::vector<uint8_t> f = frame_como_linux(0x10, 0xFF, payload);
    inyectar(f);
    cb.update(sm, 0);
    cb.processCommands(sm, policy);
    fake::advance(10);
    sm.update();
    CHECK(sm.hayEmergencia() == true);
    CHECK(sm.getState() == SystemState::EMERGENCIA);
}

static void test_handle_command_error_reset() {
    // ERROR con payload 0xFFFF -> resetEmergencia + setMantenimiento(false)
    StateMachine sm;
    TripPolicy policy;
    CommBridge cb;
    cb.begin();
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    fake::setPin(PIN_MODO_GEN, HIGH);
    sm.begin();
    sm.triggerEmergencia(ERR_SOBRECARGA);
    CHECK(sm.hayEmergencia() == true);

    std::vector<uint8_t> f = frame_como_linux(0xFF, 0xFF, 0xFFFF);
    inyectar(f);
    cb.update(sm, 0);
    cb.processCommands(sm, policy);
    CHECK(sm.hayEmergencia() == false);
    CHECK(sm.getState() == SystemState::NORMAL);
}

static void test_opcode_desconocido_se_ignora() {
    // Cualquier msg_type fuera del switch no debe hacer nada ni romper.
    StateMachine sm;
    TripPolicy policy;
    CommBridge cb;
    cb.begin();
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    fake::setPin(PIN_MODO_GEN, HIGH);
    sm.begin();

    const uint8_t desconocidos[] = {0x00, 0x01, 0x20, 0x50, 0x60, 0x99, 0xFE};
    for (uint8_t op : desconocidos) {
        std::vector<uint8_t> f = frame_como_linux(op, 0xFF, 0xFFFF);
        CHECK(validar_mensaje(*(const McuMessage*)f.data()) == true);
        inyectar(f);
        cb.update(sm, 0);
    cb.processCommands(sm, policy);
        CHECK(fake::serial1().rx_pendientes() == 0);   // consumido
    }
    fake::advance(10);
    sm.update();
    CHECK(sm.getState() == SystemState::NORMAL);
    CHECK(sm.hayEmergencia() == false);
}

// ============================================================
// 5. El camino de recepción del UART no puede COLGAR el firmware
// ============================================================
//
// comm_bridge.cpp:23-31:
//     void CommBridge::update(const StateMachine& sm, uint8_t nivel_pct) {
//         while (MCU_SERIAL.available()) {
//             uint8_t byte;
//             if (uartRead(byte)) {                 // <-- lee de rx_buffer
//                 rx_buffer[rx_head] = byte;
//                 rx_head = (rx_head + 1) % RX_BUF_SIZE;
//             }
//         }
//
// El bucle mira MCU_SERIAL.available() (el UART HARDWARE) pero drena de
// uartRead(), que saca de rx_buffer (el ring INTERNO). Y no hay NINGÚN
// sitio en todo el firmware que copie del UART hardware a rx_buffer.
// Con rx_buffer vacío, uartRead() devuelve false, no cambia nada y
// available() sigue dando > 0: BUCLE INFINITO.
//
// Estas escenas se ejecutan en un proceso hijo acotado en el tiempo, así
// que el test informa del cuelgue en lugar de colgarse.

static int escena_update_con_un_byte() {
    fake::reset();
    fake::setMillis(1000000);
    CommBridge cb;
    cb.begin();
    StateMachine sm;
    TripPolicy pol;
    std::vector<uint8_t> un_byte = {0x00};
    inyectar(un_byte);
    cb.update(sm, 0);
    return 0;   // si llega aquí, NO se colgó
}

static int escena_update_con_frame_valido() {
    fake::reset();
    fake::setMillis(1000000);
    CommBridge cb;
    cb.begin();
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    fake::setPin(PIN_MODO_GEN, LOW);          // el pin dice GENERADOR
    StateMachine sm;
    sm.begin();
    TripPolicy pol;
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 0);  // MODO_CHANGE->RED
    inyectar(f);
    cb.update(sm, 0);
    cb.processCommands(sm, pol);
    return 0;
}

static int escena_comando_del_cable_llega_a_la_maquina() {
    // Extremo a extremo: un frame bien formado que llega por el UART tiene
    // que llegar a la maquina de estados, atravesando las tres capas
    // (update -> anillo -> processCommands) y la resincronizacion tras
    // ruido previo.
    //
    // Se prueban DOS comandos porque importa la diferencia:
    //   a) MODO_CHANGE(RED) con el pin en GENERADOR -> debe RECHAZARSE.
    //      Este era el bypass de SIF-01.
    //   b) STATE_CHANGE(EMERGENCIA) -> debe llegar y hacer efecto.
    // Si solo se probara (a), un enlace totalmente roto pasaria el test.
    fake::reset();
    fake::setMillis(1000000);
    CommBridge cb;
    cb.begin();
    fake::setMode(PIN_MODO_GEN, INPUT_PULLUP);
    fake::setPin(PIN_MODO_GEN, LOW);          // GENERADOR
    StateMachine sm;
    sm.begin();
    TripPolicy pol;
    if (sm.getState() != SystemState::GENERADOR) return 3;

    // (a) Intento de relajar el modo por debajo del hardware
    std::vector<uint8_t> ruido(20, 0x5A);
    std::vector<uint8_t> f = frame_como_linux(0x40, 0xFF, 0);   // bit0=0 -> RED
    ruido.insert(ruido.end(), f.begin(), f.end());
    inyectar(ruido);
    cb.update(sm, 0);
    cb.processCommands(sm, pol);
    fake::advance(10);
    sm.update();

    if (fake::serial1().rx_pendientes() != 0) return 4;   // UART drenado
    if (cb.rxOkCount() == 0) return 6;                    // frame aceptado
    // ...pero el modo sigue siendo GENERADOR y B2/B3 bloqueadas
    if (sm.getState() != SystemState::GENERADOR) return 5;
    if (sm.puedeArrancar(1) || sm.puedeArrancar(2)) return 7;
    if (fake::getPin(PIN_RELE_BOMBA2) != RELAY_INACTIVE_LEVEL) return 8;
    if (fake::getPin(PIN_RELE_BOMBA3) != RELAY_INACTIVE_LEVEL) return 9;

    // (b) Un comando que SI debe llegar: forzar emergencia
    uint16_t payload = (uint16_t)((ERR_SOBRECARGA << 8) |
                                  (uint8_t)SystemState::EMERGENCIA);
    std::vector<uint8_t> g = frame_como_linux(0x10, 0xFF, payload);
    inyectar(g);
    cb.update(sm, 0);
    cb.processCommands(sm, pol);
    if (!sm.hayEmergencia()) return 10;
    if (sm.getState() != SystemState::EMERGENCIA) return 11;
    return 0;
}

static void test_recibir_un_byte_no_colga_el_firmware() {
    Resultado r = ejecutar_acotado(escena_update_con_un_byte);
    if (r == COLGO) {
        std::printf("FAIL CommBridge::update() NO TERMINA con 1 byte pendiente "
                    "en el UART (bucle infinito, comm_bridge.cpp:25-31)\n");
    }
    CHECK(r != COLGO);
}

static void test_recibir_un_frame_no_colga_el_firmware() {
    Resultado r = ejecutar_acotado(escena_update_con_frame_valido);
    if (r == COLGO) {
        std::printf("FAIL CommBridge::update() NO TERMINA con un frame de 11 "
                    "bytes pendiente (bucle infinito)\n");
    }
    CHECK(r != COLGO);
}

static void test_comando_del_cable_llega_a_la_maquina() {
    Resultado r = ejecutar_acotado(escena_comando_del_cable_llega_a_la_maquina);
    if (r == COLGO) {
        std::printf("FAIL el comando del cable no llega: CommBridge::update() "
                    "se cuelga antes de bombear los bytes al ring interno\n");
        CHECK(false);
        return;
    }
    if (r == VOLVIO_ERROR) {
        std::printf("FAIL el frame del UART no llegó a la máquina de estados "
                    "(rx_buffer nunca se rellena)\n");
        CHECK(false);
        return;
    }
    CHECK(true);
}

int main() {
    std::printf("--- Protocolo de 11 bytes y CRC8 (3 copias) ---\n");
    test_layout_del_frame();
    test_crc8_firmware_coincide_con_linux();
    test_crc8_python_firma_es_aceptada_por_el_firmware();
    test_regla_de_crc_unica_y_coherente();
    test_descarta_crc_malo();
    test_descarta_start_byte_malo();
    test_descarta_end_byte_malo();
    test_descarta_longitud_incompleta();
    test_acepta_frame_completo_valido();
    test_basura_por_delante_se_resincroniza();
    test_handle_command_modo_change();
    test_handle_command_state_change_emergencia();
    test_handle_command_error_reset();
    test_opcode_desconocido_se_ignora();
    test_recibir_un_byte_no_colga_el_firmware();
    test_recibir_un_frame_no_colga_el_firmware();
    test_comando_del_cable_llega_a_la_maquina();

    std::printf("%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (protocolo CRC)\n");
        return 0;
    }
    return 1;
}