/**
 * @file boot_slot.cpp
 * @brief Máquina de estados de slots A/B
 */

#include "boot_slot.h"

void BootSlotManager::setState(SlotId s, SlotState st) {
    if (s == SlotId::A) state_a_ = st;
    else state_b_ = st;
}

void BootSlotManager::restore(SlotId active, SlotState a, SlotState b,
                              uint32_t version_a, uint32_t version_b) {
    active_ = active;
    state_a_ = a;
    state_b_ = b;
    version_a_ = version_a;
    version_b_ = version_b;
    is_trial_ = false;
}

bool BootSlotManager::hayConfirmado() const {
    return state_a_ == SlotState::CONFIRMED || state_b_ == SlotState::CONFIRMED;
}

SlotId BootSlotManager::slotConfirmado() const {
    return state_a_ == SlotState::CONFIRMED ? SlotId::A : SlotId::B;
}

BootAction BootSlotManager::nextBootAction() const {
    // Un trial en curso manda sobre todo: hay que terminar ese arranque,
    // no empezar otro.
    if (is_trial_) return BootAction::BOOT_TRIAL;

    // El slot activo debería estar CONFIRMED. Si no lo está (se restauró
    // mal, o se detectó corrupción), hay que revertir.
    if (stateOf(active_) == SlotState::CONFIRMED) {
        return BootAction::BOOT_CURRENT;
    }

    if (hayConfirmado()) return BootAction::ROLLBACK;

    // Ningún slot bueno: NO arrancar nada. Los relés quedan abiertos por
    // muelle y el PNOZ sin latido. Es el estado seguro.
    return BootAction::SAFE_MODE;
}

SlotState BootSlotManager::stateAfterBoot(BootAction action) const {
    switch (action) {
        case BootAction::BOOT_CURRENT:
            return SlotState::CONFIRMED;
        case BootAction::BOOT_TRIAL:
            return SlotState::TRIAL;
        case BootAction::ROLLBACK:
            return hayConfirmado() ? SlotState::CONFIRMED : SlotState::EMPTY;
        case BootAction::SAFE_MODE:
            return SlotState::EMPTY;
    }
    return SlotState::EMPTY;
}

bool BootSlotManager::markPending(SlotId slot, uint32_t version) {
    // Escribir sobre el slot en ejecución lo dejaría inservible a mitad de
    // descarga. Es el error clásico de A/B mal implementado.
    if (slot == active_) return false;

    SlotState st = stateOf(slot);
    // Un slot TRIAL en curso no se toca: es el firmware que se está
    // auto-testando, sobrescribirlo invalidate la prueba.
    if (st == SlotState::TRIAL) return false;

    setState(slot, SlotState::PENDING);
    if (slot == SlotId::A) version_a_ = version;
    else version_b_ = version;
    return true;
}

bool BootSlotManager::armTrial(SlotId slot) {
    // Solo un slot PENDING puede armarse. Armar EMPTY (nada que arrancar),
    // CONFIRMED (ya está en marcha) o FAILED (ya se probó mal) sería dejar
    // al operador arrancar algo que no se ha validado.
    if (stateOf(slot) != SlotState::PENDING) return false;

    // No se pisan dos trials: sería imposible saber cuál auto-testear.
    if (is_trial_) return false;

    trial_slot_ = slot;
    is_trial_ = true;
    setState(slot, SlotState::TRIAL);
    return true;
}

void BootSlotManager::confirmTrial() {
    if (!is_trial_) return;
    // El slot pasa a CONFIRMED y el anterior se queda CONFIRMED también:
    // queda como destino de un rollback manual. Descartarlo tiraría la
    // única red de seguridad que queda tras una actualización.
    setState(trial_slot_, SlotState::CONFIRMED);
    active_ = trial_slot_;
    is_trial_ = false;
}

void BootSlotManager::reportTrialFailure() {
    if (!is_trial_) return;
    // Fallo: el slot queda muerto y se vuelve al confirmado.
    setState(trial_slot_, SlotState::FAILED);
    // Si no hay plan B, no hay nada que arrancar: nextBootAction() ya
    // devuelve SAFE_MODE porque ningún slot queda CONFIRMED. No hace
    // falta borrar nada, y así el marcador FAILED se conserva como
    // diagnóstico de por qué la planta quedó sin firmware bueno.
    if (hayConfirmado()) {
        active_ = slotConfirmado();
    }
    is_trial_ = false;
}

void BootSlotManager::watchdogExpired() {
    // Un firmware que no confirma en el plazo no se da por bueno, aunque no
    // haya dicho nada. Es el caso del cuelgue silencioso.
    reportTrialFailure();
}
