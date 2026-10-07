/**
 * @file test_boot_slot.cpp
 * @brief Tests nativos de la máquina de estados A/B con rollback
 *
 * Compilar y ejecutar:  make tests-native
 * Sin hardware.
 */

#include "boot_slot.h"
#include "self_test.h"

#include <cstdio>
#include <cstring>

static int checks = 0;
static int failures = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static BootSlotManager manager_confirmado(uint32_t version = 10) {
    BootSlotManager m;
    m.restore(SlotId::A, SlotState::CONFIRMED, SlotState::EMPTY, version, 0);
    return m;
}

// --- Operación normal ----------------------------------------------------

static void test_operacion_normal_bootea_slot_confirmado() {
    BootSlotManager m = manager_confirmado(10);
    CHECK(m.nextBootAction() == BootAction::BOOT_CURRENT);
    CHECK(m.stateAfterBoot(BootAction::BOOT_CURRENT) == SlotState::CONFIRMED);
    CHECK(m.activeSlot() == SlotId::A);
    CHECK(m.runningVersion() == 10);
}

static void test_estado_inicial_vacio_va_a_safe_mode() {
    // Sin ningún slot bueno, NO se arranca nada. Los relés quedan abiertos
    // por muelle y el PNOZ sin latido: es el estado seguro.
    BootSlotManager m;
    CHECK(m.nextBootAction() == BootAction::SAFE_MODE);
    CHECK(m.stateAfterBoot(BootAction::SAFE_MODE) == SlotState::EMPTY);
}

// --- La propiedad central: una imagen descargada NO arranca sola ----------

static void test_imagen_descargada_no_boota_sola() {
    // Esta es la propiedad de seguridad principal del diseño.
    BootSlotManager m = manager_confirmado(10);

    // Linux manda una imagen verificada al slot B
    CHECK(m.markPending(SlotId::B, 11));
    CHECK(m.stateOf(SlotId::B) == SlotState::PENDING);
    CHECK(m.versionOf(SlotId::B) == 11);

    // Reinicio: sigue el slot confirmado. B NO arranca.
    CHECK(m.nextBootAction() == BootAction::BOOT_CURRENT);
    CHECK(m.activeSlot() == SlotId::A);
    CHECK(m.stateOf(SlotId::B) == SlotState::PENDING);
}

static void test_activacion_exige_accion_local() {
    BootSlotManager m = manager_confirmado(10);
    m.markPending(SlotId::B, 11);

    // Antes del arming explícito, nada cambia
    CHECK(m.nextBootAction() == BootAction::BOOT_CURRENT);

    // El operador gira la llave física
    CHECK(m.armTrial(SlotId::B));
    CHECK(m.trialInProgress());
    CHECK(m.trialSlot() == SlotId::B);
    CHECK(m.stateOf(SlotId::B) == SlotState::TRIAL);

    // Ahora sí, el próximo reinicio arranca en B
    CHECK(m.nextBootAction() == BootAction::BOOT_TRIAL);
    CHECK(m.stateAfterBoot(BootAction::BOOT_TRIAL) == SlotState::TRIAL);
}

static void test_no_se_puede_armar_slot_invalido() {
    BootSlotManager m = manager_confirmado(10);

    // EMPTY: no hay nada que arrancar
    CHECK(!m.armTrial(SlotId::B));
    // CONFIRMED: ya está en marcha, armar sería reinstalar
    CHECK(!m.armTrial(SlotId::A));
    CHECK(!m.trialInProgress());
    CHECK(m.nextBootAction() == BootAction::BOOT_CURRENT);
}

static void test_no_se_pisan_dos_trials() {
    BootSlotManager m = manager_confirmado(10);
    m.markPending(SlotId::B, 11);
    CHECK(m.armTrial(SlotId::B));
    // El segundo arming se ignora: sin saber cuál auto-testear, el
    // bootloader no puede decidir
    CHECK(!m.armTrial(SlotId::B));
    CHECK(m.trialSlot() == SlotId::B);
}

// --- Protección contra sobrescribir el slot en ejecución ------------------

static void test_no_sobreescribe_slot_en_ejecucion() {
    // Escribir sobre el firmware que se está ejecutando lo deja
    // inservible a mitad de descarga. Es el error clásico de A/B.
    BootSlotManager m = manager_confirmado(10);
    CHECK(!m.markPending(SlotId::A, 11));
    CHECK(m.stateOf(SlotId::A) == SlotState::CONFIRMED);
    CHECK(m.versionOf(SlotId::A) == 10);
    // B sí se puede
    CHECK(m.markPending(SlotId::B, 11));
}

static void test_no_sobreescribe_slot_en_trial() {
    BootSlotManager m = manager_confirmado(10);
    m.markPending(SlotId::B, 11);
    m.armTrial(SlotId::B);
    // Descargar otra imagen mientras B se auto-testea invalidaría la prueba
    CHECK(!m.markPending(SlotId::B, 12));
    CHECK(m.stateOf(SlotId::B) == SlotState::TRIAL);
    CHECK(m.versionOf(SlotId::B) == 11);
}

// --- Confirmación y rollback ---------------------------------------------

static void test_confirmacion_promueve_a_confirmado() {
    BootSlotManager m = manager_confirmado(10);
    m.markPending(SlotId::B, 11);
    m.armTrial(SlotId::B);
    m.confirmTrial();

    CHECK(!m.trialInProgress());
    CHECK(m.activeSlot() == SlotId::B);
    CHECK(m.stateOf(SlotId::B) == SlotState::CONFIRMED);
    CHECK(m.runningVersion() == 11);
    CHECK(m.nextBootAction() == BootAction::BOOT_CURRENT);

    // A se conserva como destino de un rollback manual: descartar el
    // firmware anterior tiraría la única red que queda.
    CHECK(m.stateOf(SlotId::A) == SlotState::CONFIRMED);
}

static void test_fallo_hace_rollback() {
    BootSlotManager m = manager_confirmado(10);
    m.markPending(SlotId::B, 11);
    m.armTrial(SlotId::B);
    m.reportTrialFailure();

    CHECK(!m.trialInProgress());
    CHECK(m.stateOf(SlotId::B) == SlotState::FAILED);
    CHECK(m.activeSlot() == SlotId::A);
    CHECK(m.runningVersion() == 10);
    CHECK(m.nextBootAction() == BootAction::BOOT_CURRENT);

    // B queda muerto: no se reintenta solo, ni en bucle
    CHECK(!m.armTrial(SlotId::B));
}

static void test_watchdog_hace_rollback() {
    // El firmware no dice nada y se cuelga: el bootloader revierte.
    BootSlotManager m = manager_confirmado(10);
    m.markPending(SlotId::B, 11);
    m.armTrial(SlotId::B);
    m.watchdogExpired();

    CHECK(!m.trialInProgress());
    CHECK(m.stateOf(SlotId::B) == SlotState::FAILED);
    CHECK(m.activeSlot() == SlotId::A);
}

static void test_confirmar_sin_trial_no_hace_nada() {
    BootSlotManager m = manager_confirmado(10);
    m.confirmTrial();
    m.reportTrialFailure();
    m.watchdogExpired();
    CHECK(m.activeSlot() == SlotId::A);
    CHECK(m.stateOf(SlotId::A) == SlotState::CONFIRMED);
    CHECK(m.stateOf(SlotId::B) == SlotState::EMPTY);
}

static void test_rollback_si_hay_plan_b() {
    // B estaba confirmado (v5) y A estaba en trial: hay a dónde volver.
    BootSlotManager m;
    m.restore(SlotId::B, SlotState::EMPTY, SlotState::CONFIRMED, 0, 5);
    m.markPending(SlotId::A, 6);
    m.armTrial(SlotId::A);
    CHECK(m.nextBootAction() == BootAction::BOOT_TRIAL);
    m.reportTrialFailure();

    CHECK(m.stateOf(SlotId::A) == SlotState::FAILED);
    CHECK(m.activeSlot() == SlotId::B);
    CHECK(m.runningVersion() == 5);
    CHECK(m.nextBootAction() == BootAction::BOOT_CURRENT);
}

static void test_sin_plan_b_va_a_safe_mode() {
    // Despliegue de un solo slot: el único slot en uso está en trial y
    // falla. No queda nada bueno, así que no se arranca nada: los relés
    // abiertos por muelle y el PNOZ sin latido. Requiere a una persona.
    BootSlotManager m;
    m.restore(SlotId::A, SlotState::PENDING, SlotState::EMPTY, 5, 0);
    CHECK(m.armTrial(SlotId::A));
    CHECK(m.nextBootAction() == BootAction::BOOT_TRIAL);

    m.reportTrialFailure();
    CHECK(m.stateOf(SlotId::A) == SlotState::FAILED);
    CHECK(m.stateOf(SlotId::B) == SlotState::EMPTY);
    CHECK(m.nextBootAction() == BootAction::SAFE_MODE);
    CHECK(m.stateAfterBoot(BootAction::SAFE_MODE) == SlotState::EMPTY);
}

static void test_slot_corrupto_revierte() {
    // Flash corrupta: el slot activo no está CONFIRMED pero hay otro sí.
    BootSlotManager m;
    m.restore(SlotId::A, SlotState::EMPTY, SlotState::CONFIRMED, 0, 7);
    CHECK(m.nextBootAction() == BootAction::ROLLBACK);
    CHECK(m.stateAfterBoot(BootAction::ROLLBACK) == SlotState::CONFIRMED);
}

// --- Ciclos de actualización repetidos -----------------------------------

static void test_dos_actualizaciones_seguidas() {
    BootSlotManager m = manager_confirmado(10);

    // v11 a B, confirmado
    m.markPending(SlotId::B, 11);
    m.armTrial(SlotId::B);
    m.confirmTrial();
    CHECK(m.activeSlot() == SlotId::B);
    CHECK(m.runningVersion() == 11);

    // v12 vuelve a A (el que quedó como respaldo)
    CHECK(m.markPending(SlotId::A, 12));
    m.armTrial(SlotId::A);
    m.confirmTrial();
    CHECK(m.activeSlot() == SlotId::A);
    CHECK(m.runningVersion() == 12);
    CHECK(m.nextBootAction() == BootAction::BOOT_CURRENT);
}

static void test_tres_fallos_seguidos() {
    BootSlotManager m = manager_confirmado(10);
    // Un fallo deja B muerto; el siguiente intento va a A, y A era el
    // bueno, así que hay que reinstalarlo como PENDING explícitamente.
    m.markPending(SlotId::B, 11);
    m.armTrial(SlotId::B);
    m.reportTrialFailure();
    CHECK(m.activeSlot() == SlotId::A);

    // B ya no se puede rearmar sin volver a descargar
    CHECK(!m.armTrial(SlotId::B));
    m.markPending(SlotId::B, 12);
    CHECK(m.armTrial(SlotId::B));
    m.reportTrialFailure();
    CHECK(m.activeSlot() == SlotId::A);
    CHECK(m.runningVersion() == 10);
}

// --- Auto-test ------------------------------------------------------------

static void test_self_test_todo_ok() {
    SelfTest st;
    for (int i = 0; i < 5; i++) st.setResult(i, Check::PASS);
    SelfTestReport r = st.report();
    CHECK(r.passed == 5);
    CHECK(r.failed == 0);
    CHECK(r.skipped == 0);
    CHECK(r.allOk());
    CHECK(!r.anyFailed());
    CHECK(r.firstFailedIndex() == 0xFF);
}

static void test_self_test_un_fallo_no_confirma() {
    SelfTest st;
    st.setResult(0, Check::PASS);
    st.setResult(1, Check::FAIL);   // el latido del PNOZ no fluye
    st.setResult(2, Check::PASS);
    st.setResult(3, Check::PASS);
    st.setResult(4, Check::PASS);
    SelfTestReport r = st.report();
    CHECK(r.passed == 4);
    CHECK(r.failed == 1);
    CHECK(!r.allOk());
    CHECK(r.anyFailed());
    CHECK(r.firstFailedIndex() == 1);
    CHECK(r.heartbeat_generating == Check::FAIL);
}

static void test_self_test_sin_pruebas_no_confirma() {
    // Todo SKIP no es "todo bien": un firmware que no corrió las pruebas
    // no puede quedarse como firmware bueno.
    SelfTest st;
    SelfTestReport r = st.report();
    CHECK(r.passed == 0);
    CHECK(r.skipped == 5);
    CHECK(!r.allOk());
}

static void test_self_test_indice_invalido_ignorado() {
    SelfTest st;
    st.setResult(5, Check::FAIL);
    st.setResult(99, Check::FAIL);
    SelfTestReport r = st.report();
    CHECK(r.failed == 0);
    CHECK(r.skipped == 5);
}

static void test_primera_prueba_fallada_orden() {
    SelfTest st;
    st.setResult(3, Check::FAIL);
    st.setResult(0, Check::FAIL);
    st.setResult(2, Check::FAIL);
    st.setResult(1, Check::FAIL);
    st.setResult(4, Check::PASS);
    // La primera del orden es la que conviene mirar: las otras pueden ser
    // consecuencia suya.
    CHECK(st.report().firstFailedIndex() == 0);
}

static void test_umbrales_de_proteccion_sanos() {
    // Valores de config.h
    CHECK(SelfTest::protectionSane(40.0f, 42.0f, 37.0f, 45.0f));
    // Umbral a cero = protección desactivada aunque el código corra bien
    CHECK(!SelfTest::protectionSane(0.0f, 42.0f, 37.0f, 45.0f));
    CHECK(!SelfTest::protectionSane(40.0f, 0.0f, 37.0f, 45.0f));
    CHECK(!SelfTest::protectionSane(40.0f, 42.0f, 0.0f, 45.0f));
    CHECK(!SelfTest::protectionSane(40.0f, 42.0f, 37.0f, 0.0f));
    CHECK(!SelfTest::protectionSane(-1.0f, 42.0f, 37.0f, 45.0f));
    // Orden sin histeresis: el protector re-arma oscilando
    CHECK(!SelfTest::protectionSane(42.0f, 42.0f, 37.0f, 45.0f));
    CHECK(!SelfTest::protectionSane(43.0f, 42.0f, 37.0f, 45.0f));
    CHECK(!SelfTest::protectionSane(40.0f, 42.0f, 41.0f, 45.0f));
    // Por encima de la capacidad real = nunca dispara
    CHECK(!SelfTest::protectionSane(40.0f, 60.0f, 37.0f, 45.0f));
}

static void test_nombres_de_pruebas() {
    CHECK(std::strcmp(SelfTest::name(0), "relays-off-at-boot") == 0);
    for (int i = 0; i < 5; i++) {
        CHECK(SelfTest::name(i)[0] != '\0');
    }
    CHECK(SelfTest::name(99)[0] != '\0');
}

static void test_config_timeout() {
    BootSlotConfig cfg;
    cfg.confirm_timeout_ms = 30000;
    BootSlotManager m(cfg);
    CHECK(m.config().confirm_timeout_ms == 30000);
    BootSlotManager d;
    CHECK(d.config().confirm_timeout_ms == 60000);
}

int main() {
    std::printf("--- Slots A/B con rollback ---\n");
    test_operacion_normal_bootea_slot_confirmado();
    test_estado_inicial_vacio_va_a_safe_mode();
    test_imagen_descargada_no_boota_sola();
    test_activacion_exige_accion_local();
    test_no_se_puede_armar_slot_invalido();
    test_no_se_pisan_dos_trials();
    test_no_sobreescribe_slot_en_ejecucion();
    test_no_sobreescribe_slot_en_trial();
    test_confirmacion_promueve_a_confirmado();
    test_fallo_hace_rollback();
    test_watchdog_hace_rollback();
    test_confirmar_sin_trial_no_hace_nada();
    test_rollback_si_hay_plan_b();
    test_sin_plan_b_va_a_safe_mode();
    test_slot_corrupto_revierte();
    test_dos_actualizaciones_seguidas();
    test_tres_fallos_seguidos();

    std::printf("--- Auto-test de arranque ---\n");
    test_self_test_todo_ok();
    test_self_test_un_fallo_no_confirma();
    test_self_test_sin_pruebas_no_confirma();
    test_self_test_indice_invalido_ignorado();
    test_primera_prueba_fallada_orden();
    test_umbrales_de_proteccion_sanos();
    test_nombres_de_pruebas();
    test_config_timeout();

    std::printf("%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (slots A/B + auto-test)\n");
        return 0;
    }
    return 1;
}
