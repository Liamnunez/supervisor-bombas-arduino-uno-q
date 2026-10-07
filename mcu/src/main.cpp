/**
 * @file main.cpp
 * @brief Punto de entrada principal - Supervisor de Bombas Arduino UNO Q
 * 
 * STM32U585 @ 160MHz - Arduino UNO Q
 * Intercepta salidas PLC y limita bombas según modo (RED/GENERADOR)
 */

#include <Arduino.h>
#include <stddef.h>
#include "config.h"
#include "state_machine.h"
#include "relay_control.h"
#include "sensor_level.h"
#include "mode_detect.h"
#include "feedback.h"
#include "comm_bridge.h"
#include "sensor_current.h"
#include "current_protector.h"
#include "pnoz_heartbeat.h"
#include "trip_policy.h"
#include "boot_slot.h"
#include "self_test.h"

// Instancias globales
StateMachine stateMachine;
RelayControl relayControl;
SensorLevel sensorLevel;
ModeDetect modeDetect;
Feedback feedback;
CommBridge commBridge;
SensorCurrent sensorCurrent;

// Protector de corriente del generador (configurado desde config.h)
static CurrentProtector makeProtector() {
    ProtectorConfig cfg;
    cfg.warn_amps = CORRIENTE_AVISO_A;
    cfg.trip_amps = CORRIENTE_TRIP_A;
    cfg.reset_amps = CORRIENTE_RESET_A;
    cfg.welded_amps = CONTACTOR_PEGADO_A;
    cfg.ct_max_amps = CT_FAULT_MAX_A;
    cfg.trip_delay_ms = CORRIENTE_TRIP_DELAY_MS;
    cfg.warn_delay_ms = CORRIENTE_AVISO_DELAY_MS;
    cfg.reset_delay_ms = CORRIENTE_RESET_DELAY_MS;
    cfg.welded_delay_ms = CONTACTOR_PEGADO_DELAY_MS;
    cfg.ct_fault_delay_ms = CT_FAULT_DELAY_MS;
    cfg.num_relays = NUM_BOMBAS;
    return CurrentProtector(cfg);
}
CurrentProtector currentProtector = makeProtector();

// Timing
uint32_t last_loop_time = 0;
uint32_t last_block_time = 0;
const uint32_t LOOP_INTERVAL_MS = 10;  // 100Hz loop principal

// Latido al relé de seguridad PNOZ s4 (mod watchdog)
PnozHeartbeat pnozHeartbeat(PNOZ_PULSE_PERIOD_MS);

// Política de reintentos del trip (SIF-04) - el sistema se recupera solo
// hasta TRIP_MAX_ATTEMPTS intentos dentro de TRIP_WINDOW_MS; después
// espera a una persona.
static TripPolicy makeTripPolicy() {
    TripPolicyConfig cfg;
    cfg.max_attempts = TRIP_MAX_ATTEMPTS;
    cfg.window_ms = TRIP_WINDOW_MS;
    cfg.cooldown_ms = TRIP_COOLDOWN_MS;
    cfg.recover_amps = TRIP_RECOVER_AMPS;
    return TripPolicy(cfg);
}
TripPolicy tripPolicy = makeTripPolicy();

// --- Auto-test de arranque y slots A/B (Fase 4) ---
// Solo activo con FW_OTA_ENABLED. Ver docs/ota_procedure.md.
static BootSlotManager makeBootSlots() {
    BootSlotConfig cfg;
    cfg.confirm_timeout_ms = OTA_CONFIRM_TIMEOUT_MS;
    return BootSlotManager(cfg);
}
BootSlotManager bootSlots = makeBootSlots();
SelfTest selfTest;
bool selfTestConfirmed = false;
uint32_t boot_time_ms = 0;

// Watchdog
void watchdogSetup() {
    // STM32U585 IWDG - Independent Watchdog
    // LSI ~32kHz, prescaler 256 -> 125Hz -> 8ms per tick
    // Reload 625 -> 5000ms timeout
    IWDG->KR = 0x5555;  // Unlock
    IWDG->PR = 0x06;    // Prescaler 256
    IWDG->RLR = 625;    // ~5s timeout
    IWDG->KR = 0xAAAA;  // Reload
    IWDG->KR = 0xCCCC;  // Start
}

void watchdogReload() {
    IWDG->KR = 0xAAAA;
}

// Callbacks de la máquina de estados
void onStateChange(SystemState nuevo, SystemState anterior) {
    commBridge.sendStateChange(nuevo, anterior);
    // Actualizar LEDs
    digitalWrite(PIN_LED_OK, nuevo == SystemState::NORMAL);
    digitalWrite(PIN_LED_GEN, nuevo == SystemState::GENERADOR);
    digitalWrite(PIN_LED_FAULT, nuevo == SystemState::EMERGENCIA);
}

void onBombaEvent(uint8_t bomba_id, McuEvent evento) {
    commBridge.sendBombaEvent(bomba_id, evento);
}

// Retorno aux de contactor -> máquina de estados
void onFeedbackChange(uint8_t bomba_id, bool activo) {
    stateMachine.onFeedback(bomba_id, activo);
}

// Órdenes de arranque del PLC (contacto seco por bomba, anti-rebote 50ms)
void readPlcOrders() {
    static const uint8_t pins[NUM_BOMBAS] = {PIN_PLC_BOMBA1, PIN_PLC_BOMBA2, PIN_PLC_BOMBA3};
    static uint8_t cnt[NUM_BOMBAS] = {0, 0, 0};
    static bool estable[NUM_BOMBAS] = {false, false, false};

    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bool raw = (digitalRead(pins[i]) == HIGH);  // HIGH = PLC ordena arranque
        if (raw == estable[i]) {
            cnt[i] = 0;
        } else if (++cnt[i] >= (DEBOUNCE_MS / LOOP_INTERVAL_MS)) {
            estable[i] = raw;
            stateMachine.onPlcOrder(i, raw);
            cnt[i] = 0;
        }
    }
}

/**
 * Auto-test de arranque. Ver docs/ota_procedure.md §5.
 *
 * Prueba lo mínimo que hace útil el firmware: que los relés nacen
 * abiertos, que el latido del PNOZ fluye, que la máquina de estados
 * responde y que los umbrales de protección son coherentes.
 *
 * NO prueba el relé de seguridad: eso es hardware. Si el firmware no
 * emite el latido, el PNOZ abre las bombas, que es el resultado correcto.
 */
void runSelfTest() {
    // 0: los relés nacen abiertos. setup() los puso en RELAY_INACTIVE_LEVEL
    // antes de que corriera nada; si un pin quedara en HIGH el arranque
    // habría arrancado en una bomba que no fue pedida.
    bool relays_ok = (digitalRead(PIN_RELE_BOMBA1) == RELAY_INACTIVE_LEVEL) &&
                     (digitalRead(PIN_RELE_BOMBA2) == RELAY_INACTIVE_LEVEL) &&
                     (digitalRead(PIN_RELE_BOMBA3) == RELAY_INACTIVE_LEVEL);
    selfTest.setResult(0, relays_ok ? Check::PASS : Check::FAIL);

    // 1: el latido se emití3 tras un ciclo completo del loop. Se comprueba
    // en el bloque de 100 Hz, no aquí: aquí aún no ha corrido aún.
    // 2: la máquina de estados respondió al begin()
    selfTest.setResult(2, stateMachine.getState() == SystemState::NORMAL ||
                           stateMachine.getState() == SystemState::GENERADOR
                               ? Check::PASS : Check::FAIL);

    // 3: los umbrales de protección tienen que ser coherentes. Un umbral a
    // cero desactiva la protección sin que nada falle visiblemente.
    selfTest.setResult(3, SelfTest::protectionSane(
                               CORRIENTE_AVISO_A, CORRIENTE_TRIP_A,
                               CORRIENTE_RESET_A, GEN_CAPACIDAD_A)
                               ? Check::PASS : Check::FAIL);

    // 4: CRC del protocolo. Si el cálculo de crc8 está roto, la
    // comunicación con Linux falla en silencio y nadie se entera.
    McuMessage probe = {};
    probe.payload = 0x1234;
    probe.timestamp = 0xDEADBEEF;
    const size_t cuerpo = offsetof(McuMessage, crc8);
    uint8_t c1 = crc8((const uint8_t*)&probe, cuerpo);
    uint8_t c2 = crc8((const uint8_t*)&probe, cuerpo);
    // Un byte modificado tiene que cambiar el CRC: si no, el CRC es
    // constante y no detecta nada.
    probe.payload = 0x1235;
    uint8_t c3 = crc8((const uint8_t*)&probe, cuerpo);
    selfTest.setResult(4, (c1 == c2 && c1 != c3 && crc8(nullptr, 0) == 0)
                           ? Check::PASS : Check::FAIL);

    SelfTestReport r = selfTest.report();
    Serial.print("[SELFTEST] ");
    Serial.print(r.passed);
    Serial.print("/");
    Serial.print(r.passed + r.skipped);
    Serial.print(" ok, ");
    Serial.print(r.failed);
    Serial.println(" fallos");
    if (r.anyFailed()) {
        Serial.print("[SELFTEST] Fallo en: ");
        Serial.println(SelfTest::name(r.firstFailedIndex()));
    }

#if FW_OTA_ENABLED
    if (bootSlots.trialInProgress()) {
        if (r.allOk()) {
            bootSlots.confirmTrial();
            Serial.println("[SELFTEST] Slot confirmado");
        } else {
            bootSlots.reportTrialFailure();
            Serial.println("[SELFTEST] Auto-test fallo: rollback");
        }
    }
#endif
    selfTestConfirmed = r.allOk();
}

void setup() {
    // Inicialización temprana de pines críticos (fail-safe)
    pinMode(PIN_RELE_BOMBA1, OUTPUT);
    pinMode(PIN_RELE_BOMBA2, OUTPUT);
    pinMode(PIN_RELE_BOMBA3, OUTPUT);
    digitalWrite(PIN_RELE_BOMBA1, RELAY_INACTIVE_LEVEL);
    digitalWrite(PIN_RELE_BOMBA2, RELAY_INACTIVE_LEVEL);
    digitalWrite(PIN_RELE_BOMBA3, RELAY_INACTIVE_LEVEL);

    // Latido PNOZ s4 en LOW antes de nada: "sin pulso" -> el relé de
    // seguridad mantiene los relés de bomba abiertos hasta que el firmware
    // demuestre estar vivo.
    pinMode(PIN_PNOZ_HEARTBEAT, OUTPUT);
    digitalWrite(PIN_PNOZ_HEARTBEAT, LOW);

    // LEDs
    pinMode(PIN_LED_OK, OUTPUT);
    pinMode(PIN_LED_GEN, OUTPUT);
    pinMode(PIN_LED_FAULT, OUTPUT);
    digitalWrite(PIN_LED_OK, LOW);
    digitalWrite(PIN_LED_GEN, LOW);
    digitalWrite(PIN_LED_FAULT, LOW);

    // Serial para debug
    Serial.begin(115200);
    while (!Serial && millis() < 2000);  // Wait max 2s for USB CDC
    Serial.println("\n=== Supervisor de Bombas Arduino UNO Q ===");
    Serial.println("STM32U585 @ 160MHz");
    Serial.println("Build: " __DATE__ " " __TIME__);

    // Watchdog
    watchdogSetup();

    // Inicializar módulos
    relayControl.begin();
    sensorLevel.begin();
    modeDetect.begin();
    feedback.begin();
    commBridge.begin();
    sensorCurrent.begin();

    // Configurar callbacks
    stateMachine.onStateChange(onStateChange);
    stateMachine.onBombaEvent(onBombaEvent);
    feedback.onChange(onFeedbackChange);

    // Inicializar máquina de estados
    stateMachine.begin();

    // Sembrar retornos aux iniciales (evita falso fault si hay una bomba
    // en marcha desde antes de arrancar el MCU)
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        stateMachine.onFeedback(i, feedback.getFeedback(i));
    }

    // --- Auto-test de arranque (Fase 4) ---
    // Un firmware en TRIAL no se da por bueno hasta que esto pasa. Es lo
    // que la firma NO puede comprobar: una imagen auténtica puede tener
    // un fallo que solo aparece al arrancar.
    boot_time_ms = millis();
    runSelfTest();

    Serial.println("[MAIN] Inicialización completa. Entrando en loop principal.");
    last_loop_time = millis();
    last_block_time = millis();
}

void loop() {
    uint32_t now = millis();
    
    // Loop a intervalo fijo (100Hz)
    // Si el bloque de 100Hz se retrasa (bloqueo del loop), el pin de latido
    // puede quedarse en HIGH colgado. Forzarlo a LOW hace que el PNOZ
    // detecte ausencia de pulso y abra los relés - que es el lado seguro.
    if (now - last_block_time >= PNOZ_STALE_MS) {
        digitalWrite(PIN_PNOZ_HEARTBEAT, LOW);
    }

    if (now - last_loop_time >= LOOP_INTERVAL_MS) {
        last_loop_time = now;
        last_block_time = now;

        // Recargar watchdog
        watchdogReload();

        // Latido al PNOZ s4 (debe ser lo primero: es la barrier que
        // depende de que el firmware siga vivo)
        digitalWrite(PIN_PNOZ_HEARTBEAT,
                     pnozHeartbeat.update(now) ? HIGH : LOW);

        // Leer entradas
        modeDetect.update();
        feedback.update();
        sensorLevel.update();
        readPlcOrders();  // Órdenes de arranque del PLC -> StateMachine

        // Actualizar máquina de estados (lee PLC, modo, feedback, decide relés)
        stateMachine.update();

        // Aplicar decisiones de relés
        relayControl.apply(stateMachine);

        // --- Protección de corriente (CT propio, no toca el DSE 7320) ---
        float amps = sensorCurrent.readAmps();
        bool relays[NUM_BOMBAS];
        for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
            relays[i] = relayControl.getRelayState(i);
        }
        bool trip_este_ciclo = false;
        switch (currentProtector.update(now, amps, relays)) {
            case ProtectorEvent::TRIP_OVERLOAD:
                // Sobrecarga sostenida: EMERGENCIA (relés abiertos). La
                // política decide después si se recupera sola o espera.
                trip_este_ciclo = true;
                stateMachine.triggerEmergencia(ERR_SOBRECARGA);
                commBridge.sendError(ERR_SOBRECARGA);
                Serial.print("[PROT] TRIP sobrecarga generador: ");
                Serial.print(amps, 1);
                Serial.print("A (intento ");
                Serial.print(tripPolicy.attempts() + 1);
                Serial.print("/");
                Serial.print(TRIP_MAX_ATTEMPTS);
                Serial.println(")");
                break;
            case ProtectorEvent::WARN_OVERLOAD:
                commBridge.sendError(ERR_AVISO_SOBRECARGA);
                Serial.print("[PROT] Aviso sobrecarga: ");
                Serial.print(amps, 1);
                Serial.println("A");
                break;
            case ProtectorEvent::CONTACTOR_WELDED:
                commBridge.sendError(ERR_CONTACTOR_PEGADO);
                Serial.println("[PROT] Corriente con relés abiertos (contactor pegado?)");
                break;
            case ProtectorEvent::CT_FAULT:
                commBridge.sendError(ERR_SENSOR_CORRIENTE);
                Serial.println("[PROT] Sensor de corriente fuera de rango");
                break;
            default:
                break;
        }
        // --- Política de trip (Fase 5): recuperar solo o esperar operador ---
        // El trip NUNCA se re-arma mientras haya emergencia activa: durante
        // el enfriamiento y tras agotar los intentos debe seguir abierto.
        switch (tripPolicy.update(now, trip_este_ciclo, amps)) {
            case TripDecision::RECOVER:
                if (stateMachine.autoRecoverTrip()) {
                    currentProtector.rearm();
                    commBridge.sendError(ERR_TRIP_AUTO_RECOVER);
                    Serial.print("[PROT] Auto-recuperado tras ");
                    Serial.print(tripPolicy.attempts());
                    Serial.print(" intento(s). Quedan ");
                    Serial.print(tripPolicy.remaining());
                    Serial.println(" antes de exigir operador");
                }
                break;
            case TripDecision::HOLD:
                // Los intentos están agotados: la emergencia queda latched.
                // El re-arm de más abajo queda bloqueado por hayEmergencia().
                commBridge.sendError(ERR_TRIP_LATCH);
                Serial.print("[PROT] Trip ");
                Serial.print(tripPolicy.attempts());
                Serial.println(" intentos en la ventana: ESPERA OPERADOR");
                break;
            default:
                break;
        }

        // Telemetría: nº de intentos de trip en la ventana (solo lectura
        // para el dashboard; no añade campos al protocolo de 11 bytes).
        stateMachine.setTripIntentos(tripPolicy.attempts());

        // Re-armar el trip solo cuando no hay emergencia activa
        if (!stateMachine.hayEmergencia()) {
            currentProtector.rearm();
        }

        // --- SIF-05: sensor de nivel fuera de rango ---
        // Se AVISA pero NO se paran las bombas, y esa es una decisión
        // deliberada: si el sensor falla con el pozo llenándose, parar las
        // bombas empeora el problema. El peor resultado posible aquí es
        // desbordar el pozo, que es menos grave que el apagón del
        // generador que sí evitan el resto de las SIF.
        //
        // security_protocols.md decía "Alerta + safe state" sin definir qué
        // era ese safe state. La definición honesta es DEGRADADO: las
        // bombas siguen las órdenes del PLC (que es quien decide por nivel)
        // y el operador recibe un crítico de que la lectura no es de fiar.
        static bool nivel_sano_antes = true;
        if (sensorLevel.isValid() != nivel_sano_antes) {
            nivel_sano_antes = sensorLevel.isValid();
            if (!nivel_sano_antes) {
                stateMachine.setNivelSensorFallo(true);
                commBridge.sendError(ERR_SENSOR_NIVEL);
                Serial.println("[SIF05] Sensor de nivel fuera de rango "
                              "(3.5-21mA): lectura NO confiable, bombas "
                              "siguen las ordenes del PLC");
            } else {
                stateMachine.setNivelSensorFallo(false);
                Serial.println("[SIF05] Sensor de nivel recuperado");
            }
        }

        // Comunicación MCU <-> Linux
        commBridge.update(stateMachine, sensorLevel.getNivelPorcentaje());

        // Heartbeat periódico + telemetría de corriente (1Hz)
        static uint32_t last_hb = 0;
        if (now - last_hb >= HEARTBEAT_MS) {
            last_hb = now;
            commBridge.sendHeartbeat(stateMachine.getState(), sensorLevel.getNivelPorcentaje(), sensorLevel.isValid());
            commBridge.sendCorriente(amps);
        }
    }

    // Procesar comandos desde Linux (no bloqueante)
    commBridge.processCommands(stateMachine, tripPolicy);
}