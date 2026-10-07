/**
 * @file self_test.cpp
 * @brief Auto-test de arranque
 */

#include "self_test.h"

void SelfTest::setResult(uint8_t index, Check result) {
    if (index >= 5) return;
    results_[index] = result;
}

SelfTestReport SelfTest::report() const {
    SelfTestReport r;
    for (int i = 0; i < 5; i++) {
        switch (results_[i]) {
            case Check::PASS:  r.passed++; break;
            case Check::FAIL:  r.failed++; break;
            default:           r.skipped++; break;
        }
    }
    r.relays_off_at_boot = results_[0];
    r.heartbeat_generating = results_[1];
    r.state_machine_reachable = results_[2];
    r.protection_configured = results_[3];
    r.protocol_crc_ok = results_[4];
    return r;
}

uint8_t SelfTestReport::firstFailedIndex() const {
    // Mismo orden que SelfTest::setResult(). La primera que falla es la que
    // conviene mirar primero: el resto puede ser consecuencia suya.
    if (relays_off_at_boot == Check::FAIL) return 0;
    if (heartbeat_generating == Check::FAIL) return 1;
    if (state_machine_reachable == Check::FAIL) return 2;
    if (protection_configured == Check::FAIL) return 3;
    if (protocol_crc_ok == Check::FAIL) return 4;
    return 0xFF;
}

bool SelfTest::protectionSane(float warn_a, float trip_a, float reset_a,
                              float capacity_a) {
    // Un umbral a cero o negativo desactivaría la protección sin que nada
    // falle visiblemente: el código corre "bien" y no protege nada.
    if (warn_a <= 0.0f || trip_a <= 0.0f || reset_a <= 0.0f || capacity_a <= 0.0f) {
        return false;
    }
    // Orden correcto: aviso por debajo de trip, y el re-arm por debajo del
    // aviso (histeresis). Sin esto el protector re-arma oscilando.
    if (warn_a >= trip_a) return false;
    if (reset_a >= warn_a) return false;
    // Ningún umbral puede estar por encima de la capacidad real.
    if (trip_a > capacity_a) return false;
    if (warn_a > capacity_a) return false;
    return true;
}

const char* SelfTest::name(uint8_t index) {
    switch (index) {
        case 0: return "relays-off-at-boot";
        case 1: return "heartbeat-generating";
        case 2: return "state-machine-reachable";
        case 3: return "protection-configured";
        case 4: return "protocol-crc-ok";
        default: return "desconocida";
    }
}
