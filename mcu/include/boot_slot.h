/**
 * @file boot_slot.h
 * @brief Máquina de estados de slots A/B con rollback automático
 *
 * Un firmware que arranca en modo TRIAL no se da por bueno hasta que se
 * auto-testea. Si no confirma a tiempo, el bootloader lo revierte al slot
 * confirmado. Un firmware que se cuelga antes de confirmar NUNCA queda
 * activo.
 *
 * Eso resuelve el problema que la firma NO resuelve: una imagen auténtica y
 * bien firmada puede tener un fallo que solo aparece al arrancar (índice
 * fuera de rango en una tabla, peripheral mal configurado, presupuesto de
 * pila insuficiente). La firma demuestra autenticidad, no que arranque.
 *
 * El auto-test nunca prueba el relé de seguridad: eso es hardware. Si el
 * firmware no arranca el latido, el PNOZ abre las bombas - que es el
 * resultado correcto, no un fallo.
 *
 * Lógica pura (solo <stdint.h>) para testearse nativamente con g++ en
 * tests/native/ - sin hardware.
 */

#ifndef BOOT_SLOT_H
#define BOOT_SLOT_H

#include <stdint.h>

enum class SlotId : uint8_t {
    A = 0,
    B = 1,
};

enum class SlotState : uint8_t {
    EMPTY = 0,     // sin imagen
    PENDING,       // imagen verificada y guardada, NO activada
    TRIAL,         // arrancando desde este slot, sin confirmar
    CONFIRMED,     // auto-test superado: es el bueno
    FAILED,        // auto-test falló o no confirmó a tiempo
};

/** Qué debe hacer el bootloader en el próximo reinicio */
enum class BootAction : uint8_t {
    BOOT_CURRENT = 0,  // slot confirmado (caso normal)
    BOOT_TRIAL,        // arrancar en el slot armado por el operador
    ROLLBACK,          // revertir al slot confirmado
    SAFE_MODE,         // nada arrancable: relés abiertos, aviso al operador
};

struct BootSlotConfig {
    /** Margen para el bootloader antes de revertir si nadie confirma. */
    uint32_t confirm_timeout_ms = 60000;
};

class BootSlotManager {
public:
    BootSlotManager() {}
    explicit BootSlotManager(const BootSlotConfig& cfg) : cfg_(cfg) {}

    /** Restaura el estado persistido en flash. */
    void restore(SlotId active, SlotState a, SlotState b,
                 uint32_t version_a, uint32_t version_b);

    /** Qué hacer en el próximo reinicio. */
    BootAction nextBootAction() const;

    /** Estado en que quedará el slot tras la acción indicada. */
    SlotState stateAfterBoot(BootAction action) const;

    /**
     * Una imagen verificada se ha guardado en un slot. NO se activa.
     * @return false si el slot no es válido o es el que está en ejecución
     *         (escribir sobre el firmware en uso lo dejaría inservible)
     */
    bool markPending(SlotId slot, uint32_t version);

    /**
     * El operador permite arrancar en ese slot. Es la fronteira de
     * seguridad: solo se llama desde una llave/botón físico, nunca desde un
     * comando remoto.
     * @return false si el slot no está PENDING o ya hay un trial en curso
     */
    bool armTrial(SlotId slot);

    /** El firmware en TRIAL superó su auto-test. */
    void confirmTrial();

    /** El firmware en TRIAL reportó fallo. */
    void reportTrialFailure();

    /** El bootloader venció el tiempo de confirmación. */
    void watchdogExpired();

    SlotId activeSlot() const { return active_; }
    SlotState stateOf(SlotId s) const {
        return (s == SlotId::A) ? state_a_ : state_b_;
    }
    uint32_t versionOf(SlotId s) const {
        return (s == SlotId::A) ? version_a_ : version_b_;
    }
    bool trialInProgress() const { return is_trial_; }
    SlotId trialSlot() const { return trial_slot_; }

    /** Versión del firmware en ejecución. */
    uint32_t runningVersion() const { return versionOf(active_); }

    const BootSlotConfig& config() const { return cfg_; }

private:
    BootSlotConfig cfg_;
    SlotId active_ = SlotId::A;
    SlotState state_a_ = SlotState::EMPTY;
    SlotState state_b_ = SlotState::EMPTY;
    uint32_t version_a_ = 0;
    uint32_t version_b_ = 0;
    SlotId trial_slot_ = SlotId::A;
    bool is_trial_ = false;

    void setState(SlotId s, SlotState st);
    bool hayConfirmado() const;
    SlotId slotConfirmado() const;
};

#endif // BOOT_SLOT_H
