/**
 * @file state_machine.cpp
 * @brief Máquina de estados principal - Supervisor de Bombas
 * 
 * Estados: NORMAL (RED), GENERADOR, EMERGENCIA, MANTENIMIENTO
 * Transiciones basadas en modo hardware, feedback, fallos
 */

#include "state_machine.h"
#include "config.h"
#include <Arduino.h>

StateMachine::StateMachine() 
    : current_state(SystemState::NORMAL)
    , previous_state(SystemState::NORMAL)
    , boot_time(0)
    , last_heartbeat(0)
    , last_mode_check(0)
    , modo_generador_hw(false)
    , modo_mantenimiento(false)
    , emergencia_activa(false)
    , emergencia_codigo(0)
    , state_change_cb(nullptr)
    , bomba_event_cb(nullptr)
    , modo_debounce_cnt(0)
    , modo_estable(false)
{
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bombas[i] = {};
    }
}

void StateMachine::begin() {
    boot_time = millis();
    last_heartbeat = boot_time;
    last_mode_check = boot_time;
    
    // Lectura inicial del modo hardware
    // PIN_MODO_GEN: HIGH = RED, LOW = GENERADOR (pull-up interno)
    modo_generador_hw = !digitalRead(PIN_MODO_GEN);
    readModoHardware();
    modo_estable = true;
    
    // Estado inicial según modo detectado
    if (modo_generador_hw) {
        current_state = SystemState::GENERADOR;
    } else {
        current_state = SystemState::NORMAL;
    }
    previous_state = current_state;
    
    // Notificar estado inicial
    if (state_change_cb) {
        state_change_cb(current_state, SystemState::NORMAL);
    }
}

void StateMachine::update() {
    // 1. Leer modo hardware con anti-rebote
    readModoHardware();
    
    // 2. Verificar timeouts de feedback
    checkFeedbackTimeout();
    
    // 3. Calcular estado deseado
    SystemState desired = computeDesiredState();
    
    // 4. Transición si cambió
    if (desired != current_state) {
        transitionTo(desired);
    }
    
    // 5. Lógica específica por estado
    switch (current_state) {
        case SystemState::NORMAL:
            handleNormal();
            break;
        case SystemState::GENERADOR:
            handleGenerador();
            break;
        case SystemState::EMERGENCIA:
            handleEmergencia();
            break;
        case SystemState::MANTENIMIENTO:
            handleMantenimiento();
            break;
    }
    
    // 6. Actualizar salidas de relés según estado y órdenes PLC
    updateBombas();
}

bool StateMachine::puedeArrancar(uint8_t bomba_id) const {
    if (bomba_id >= NUM_BOMBAS) return false;
    
    // En emergencia o mantenimiento: nunca
    if (current_state == SystemState::EMERGENCIA || 
        current_state == SystemState::MANTENIMIENTO) {
        return false;
    }
    
    // En generador: solo bomba 1 (id 0)
    if (current_state == SystemState::GENERADOR && bomba_id != 0) {
        return false;
    }
    
    // Verificar que no haya fault activo en esta bomba
    if (bombas[bomba_id].fault_count > 0) {
        return false;
    }
    
    return true;
}

void StateMachine::onFeedback(uint8_t bomba_id, bool activo) {
    if (bomba_id >= NUM_BOMBAS) return;
    
    bool anterior = bombas[bomba_id].feedback;
    bombas[bomba_id].feedback = activo;
    bombas[bomba_id].last_change = millis();
    
    // Detectar mismatch: PLC ordena pero no hay feedback (o viceversa)
    // El FAULT como tal lo decide checkFeedbackTimeout (con debounce y
    // latch, y sobre el estado real del relé) - aquí solo notificamos
    // el evento de telemetría. Dar fault aquí producía falsos positivos
    // al arrancar el MCU con una bomba ya en marcha.
    if (activo != bombas[bomba_id].plc_order) {
        if (bomba_event_cb) {
            bomba_event_cb(bomba_id, McuEvent::FEEDBACK_MISMATCH);
        }
    }
    
    // Notificar arranque/parada confirmada
    if (activo && !anterior) {
        if (bomba_event_cb) bomba_event_cb(bomba_id, McuEvent::BOMBA_START);
    } else if (!activo && anterior) {
        if (bomba_event_cb) bomba_event_cb(bomba_id, McuEvent::BOMBA_STOP);
    }
}

void StateMachine::onPlcOrder(uint8_t bomba_id, bool orden) {
    if (bomba_id >= NUM_BOMBAS) return;
    bombas[bomba_id].plc_order = orden;
    bombas[bomba_id].last_change = millis();
}

bool StateMachine::setModoGenerador(bool es_generador) {
    // Solo permite forzar si no hay emergencia activa
    if (emergencia_activa) return false;
    
    modo_generador_hw = es_generador;
    modo_estable = true;
    modo_debounce_cnt = 0;
    return true;
}

void StateMachine::triggerEmergencia(uint16_t codigo_error) {
    emergencia_activa = true;
    emergencia_codigo = codigo_error;
    // Forzar transición inmediata
    transitionTo(SystemState::EMERGENCIA);
}

void StateMachine::resetEmergencia() {
    if (!emergencia_activa) {
        // Sin emergencia: el reset solo limpia latch de fallos
        clearFaults();
        return;
    }
    emergencia_activa = false;
    emergencia_codigo = 0;
    // Salir de EMERGENCIA (transitionTo limpia fault_count de las bombas)
    transitionTo(computeDesiredState());
}

void StateMachine::clearFaults() {
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bombas[i].fault_count = 0;
        bombas[i].fault_latched = false;
    }
}

void StateMachine::setMantenimiento(bool activo) {
    modo_mantenimiento = activo;
    if (activo && !emergencia_activa) {
        transitionTo(SystemState::MANTENIMIENTO);
    } else if (!activo && !emergencia_activa) {
        // Volver al estado según modo hardware
        transitionTo(modo_generador_hw ? SystemState::GENERADOR : SystemState::NORMAL);
    }
}

void StateMachine::transitionTo(SystemState new_state) {
    previous_state = current_state;
    current_state = new_state;
    
    // Reset contadores de fault al salir de emergencia
    if (previous_state == SystemState::EMERGENCIA && new_state != SystemState::EMERGENCIA) {
        for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
            bombas[i].fault_count = 0;
        }
        emergencia_activa = false;
        emergencia_codigo = 0;
    }
    
    if (state_change_cb) {
        state_change_cb(current_state, previous_state);
    }
}

SystemState StateMachine::computeDesiredState() {
    // Prioridad 1: Emergencia forzada
    if (emergencia_activa) return SystemState::EMERGENCIA;
    
    // Prioridad 2: Modo mantenimiento
    if (modo_mantenimiento) return SystemState::MANTENIMIENTO;
    
    // Prioridad 3: Modo hardware (RED/GENERADOR)
    return modo_generador_hw ? SystemState::GENERADOR : SystemState::NORMAL;
}

void StateMachine::handleNormal() {
    // Modo RED: passthrough total, todas las bombas permitidas
    // La lógica de relés se maneja en updateBombas()
}

void StateMachine::handleGenerador() {
    // Modo GENERADOR: solo bomba 1 permitida
    // Bombas 2 y 3 se bloquean en updateBombas()
}

void StateMachine::handleEmergencia() {
    // Emergencia: todos los relés abiertos (hecho en updateBombas)
    // Parpadeo LED fault
    static uint32_t last_blink = 0;
    if (millis() - last_blink >= 200) {
        last_blink = millis();
        digitalWrite(PIN_LED_FAULT, !digitalRead(PIN_LED_FAULT));
    }
}

void StateMachine::handleMantenimiento() {
    // Mantenimiento: todos los relés abiertos
    // LED OK parpadea lento
    static uint32_t last_blink = 0;
    if (millis() - last_blink >= 1000) {
        last_blink = millis();
        digitalWrite(PIN_LED_OK, !digitalRead(PIN_LED_OK));
    }
}

void StateMachine::updateBombas() {
    uint32_t now = millis();
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        bool debe_cerrar = false;
        
        if (puedeArrancar(i)) {
            // El relé sigue la orden del PLC (passthrough condicionado)
            debe_cerrar = bombas[i].plc_order;
        } else {
            // No puede arrancar: relé abierto (seguridad)
            debe_cerrar = false;
        }
        
        // Registrar el instante de cada cambio de salida (base de tiempos
        // para checkFeedbackTimeout)
        if (debe_cerrar != bombas[i].relay_closed) {
            bombas[i].relay_closed = debe_cerrar;
            bombas[i].relay_change = now;
        }
    }
}

void StateMachine::checkFeedbackTimeout() {
    uint32_t now = millis();
    
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        BombaStatus& b = bombas[i];
        
        // Solo evaluamos cuando el relé lo manda NOSOTROS:
        //  - orden sin retorno: energizamos y el contactor no confirma
        //  - retorno sin orden: relé abierto pero el contactor sigue cerrado
        //    (pegado/soldado)
        // IMPORTANTE: si el modo bloquea la bomba (relé abierto por decisión
        // nuestra) NO hay fallo - es comportamiento esperado en GENERADOR.
        bool orden_sin_retorno = b.relay_closed && !b.feedback;
        bool retorno_sin_orden = !b.relay_closed && b.feedback;
        
        if (!orden_sin_retorno && !retorno_sin_orden) {
            // Condición limpia: libera el latch para el próximo episodio
            b.fault_latched = false;
            continue;
        }
        
        // Un SOLO fault por episodio (no incrementar en cada loop 100Hz)
        if (!b.fault_latched && (now - b.relay_change >= FEEDBACK_TIMEOUT_MS)) {
            b.fault_latched = true;
            b.fault_count++;
            if (bomba_event_cb) {
                bomba_event_cb(i, McuEvent::BOMBA_FAULT);
            }
            // La bomba queda bloqueada (fault_count > 0) hasta reset de operador
        }
    }
}

void StateMachine::sendHeartbeat() {
    // Se envía desde main loop via commBridge
}

void StateMachine::readModoHardware() {
    // Leer pin modo con pull-up: HIGH=RED, LOW=GENERADOR
    bool lectura = digitalRead(PIN_MODO_GEN);
    bool es_generador = !lectura;  // LOW = GENERADOR (igual que ModeDetect)
    
    // Anti-rebote simple
    if (es_generador == modo_generador_hw) {
        modo_debounce_cnt = 0;
    } else {
        modo_debounce_cnt++;
        if (modo_debounce_cnt >= 5) {  // 5 lecturas consistentes = 50ms @ 100Hz
            modo_generador_hw = es_generador;
            modo_estable = true;
            modo_debounce_cnt = 0;
            
            // Notificar cambio de modo
            if (bomba_event_cb) {
                bomba_event_cb(0xFF, McuEvent::MODO_CHANGE);
            }
        }
    }
}

uint8_t StateMachine::leerNivelAgua() {
    // Delegado a sensorLevel
    return 0;  // Placeholder
}

void StateMachine::printState() const {
    const char* state_names[] = {"NORMAL", "GENERADOR", "EMERGENCIA", "MANTENIMIENTO"};
    Serial.print("[STATE] Estado: ");
    Serial.print(state_names[static_cast<uint8_t>(current_state)]);
    Serial.print(" | Modo HW: ");
    Serial.print(modo_generador_hw ? "GEN" : "RED");
    Serial.print(" | Mantenimiento: ");
    Serial.print(modo_mantenimiento ? "SI" : "NO");
    Serial.print(" | Emergencia: ");
    Serial.println(emergencia_activa ? "SI" : "NO");
    
    for (uint8_t i = 0; i < NUM_BOMBAS; i++) {
        Serial.print("  B");
        Serial.print(i+1);
        Serial.print(": PLC=");
        Serial.print(bombas[i].plc_order);
        Serial.print(" FB=");
        Serial.print(bombas[i].feedback);
        Serial.print(" REL=");
        Serial.print(bombas[i].relay_closed);
        Serial.print(" FLT=");
        Serial.println(bombas[i].fault_count);
    }
}