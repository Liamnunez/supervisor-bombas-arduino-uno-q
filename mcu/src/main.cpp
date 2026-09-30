/**
 * @file main.cpp
 * @brief Punto de entrada principal - Supervisor de Bombas Arduino UNO Q
 * 
 * STM32U585 @ 160MHz - Arduino UNO Q
 * Intercepta salidas PLC y limita bombas según modo (RED/GENERADOR)
 */

#include <Arduino.h>
#include "config.h"
#include "state_machine.h"
#include "relay_control.h"
#include "sensor_level.h"
#include "mode_detect.h"
#include "feedback.h"
#include "comm_bridge.h"

// Instancias globales
StateMachine stateMachine;
RelayControl relayControl;
SensorLevel sensorLevel;
ModeDetect modeDetect;
Feedback feedback;
CommBridge commBridge;

// Timing
uint32_t last_loop_time = 0;
const uint32_t LOOP_INTERVAL_MS = 10;  // 100Hz loop principal

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

void setup() {
    // Inicialización temprana de pines críticos (fail-safe)
    pinMode(PIN_RELE_BOMBA1, OUTPUT);
    pinMode(PIN_RELE_BOMBA2, OUTPUT);
    pinMode(PIN_RELE_BOMBA3, OUTPUT);
    digitalWrite(PIN_RELE_BOMBA1, RELAY_INACTIVE_LEVEL);
    digitalWrite(PIN_RELE_BOMBA2, RELAY_INACTIVE_LEVEL);
    digitalWrite(PIN_RELE_BOMBA3, RELAY_INACTIVE_LEVEL);

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

    // Configurar callbacks
    stateMachine.onStateChange(onStateChange);
    stateMachine.onBombaEvent(onBombaEvent);

    // Inicializar máquina de estados
    stateMachine.begin();

    Serial.println("[MAIN] Inicialización completa. Entrando en loop principal.");
    last_loop_time = millis();
}

void loop() {
    uint32_t now = millis();
    
    // Loop a intervalo fijo (100Hz)
    if (now - last_loop_time >= LOOP_INTERVAL_MS) {
        last_loop_time = now;

        // Recargar watchdog
        watchdogReload();

        // Leer entradas
        modeDetect.update();
        feedback.update();
        sensorLevel.update();

        // Actualizar máquina de estados (lee PLC, modo, feedback, decide relés)
        stateMachine.update();

        // Aplicar decisiones de relés
        relayControl.apply(stateMachine);

        // Comunicación MCU <-> Linux
        commBridge.update(stateMachine, sensorLevel.getNivelPorcentaje());

        // Heartbeat periódico
        static uint32_t last_hb = 0;
        if (now - last_hb >= HEARTBEAT_MS) {
            last_hb = now;
            commBridge.sendHeartbeat(stateMachine.getState(), sensorLevel.getNivelPorcentaje());
        }
    }

    // Procesar comandos desde Linux (no bloqueante)
    commBridge.processCommands(stateMachine);
}