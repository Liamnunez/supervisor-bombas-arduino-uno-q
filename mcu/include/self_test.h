/**
 * @file self_test.h
 * @brief Auto-test de arranque para un firmware en modo TRIAL
 *
 * Un firmware recién flasheado no se da por bueno porque su firma sea
 * válida: se da por bueno porque arranca y hace lo mínimo que lo hace útil.
 * Si no supera estas pruebas, el bootloader revierte al slot anterior.
 *
 * Lo que NO se prueba aquí (y por qué):
 *
 *   - El relé de seguridad PNOZ: es hardware. Si el firmware no emite el
 *     latido, el PNOZ abre las bombas - eso es el resultado correcto.
 *   - Que las bombas arranquen: exige la planta.
 *   - Los umbrales de corriente: exigen calibración con carga real.
 *
 * Lo que sí se prueba es que el firmware no está corrupto ni mal
 * inicializado. Que un fallo de memoria o un periférico mal configurado no
 * dejen el supervisor en un estado en el que parece funcionar pero no
 * protege.
 *
 * Lógica pura (solo <stdint.h>) para testearse nativamente con g++ en
 * tests/native/ - sin hardware.
 */

#ifndef SELF_TEST_H
#define SELF_TEST_H

#include <stdint.h>

/** Resultado individual de una prueba */
enum class Check : uint8_t {
    SKIP = 0,
    PASS,
    FAIL,
};

struct SelfTestReport {
    Check relays_off_at_boot = Check::SKIP;
    Check heartbeat_generating = Check::SKIP;
    Check state_machine_reachable = Check::SKIP;
    Check protection_configured = Check::SKIP;
    Check protocol_crc_ok = Check::SKIP;

    uint8_t passed = 0;
    uint8_t failed = 0;
    uint8_t skipped = 0;
    uint8_t total() const { return passed + failed + skipped; }

    /** Todas las pruebas obligatorias pasaron. */
    bool allOk() const { return failed == 0 && passed > 0; }
    /** Hubo algún fallo (no se puede confirmar el slot). */
    bool anyFailed() const { return failed > 0; }
    uint8_t firstFailedIndex() const;
};

class SelfTest {
public:
    SelfTest() {}

    /** Registra el resultado de una prueba por índice (0..4). */
    void setResult(uint8_t index, Check result);

    /** Recuento de la última lectura de resultados. */
    SelfTestReport report() const;

    /**
     * Coherencia de los umbrales de protección. Separado del resto porque
     * depende de configuración, no de ejecución: un umbral a 0 significa
     * que la protección está desactivada aunque el código corra bien.
     */
    static bool protectionSane(float warn_a, float trip_a, float reset_a,
                                float capacity_a);

    /** Texto de una prueba por índice (diagnóstico). */
    static const char* name(uint8_t index);

private:
    Check results_[5] = {Check::SKIP, Check::SKIP, Check::SKIP,
                         Check::SKIP, Check::SKIP};
};

#endif // SELF_TEST_H
