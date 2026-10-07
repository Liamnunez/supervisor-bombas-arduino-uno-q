/**
 * @file fw_image.h
 * @brief Manifiesto de imagen de firmware + pipeline de verificación
 *
 * Formato de la imagen (ver docs/ota_procedure.md §4):
 *
 *   0x00  4   magic          0x4D495746 = bytes "FWIM"
 *   0x04  4   header_len     = 64
 *   0x08  4   version        monotónica, nunca decrece
 *   0x0C  4   payload_len    bytes de firmware
 *   0x10  4   build_id       git short sha o timestamp
 *   0x14  4   payload_crc32
 *   0x18  32  payload_sha256
 *   0x38  4   header_crc32   CRC32 de 0x00..0x37
 *   0x3C  4   reserved
 *   0x40  N   payload
 *   0x40+N 64 signature      Ed25519 sobre (0x00..0x3B || payload)
 *
 * ORDEN DE VERIFICACIÓN - deliberado:
 *
 *   1. magic / header_len / payload_len / longitud total  (bounds)
 *   2. versión monotónica                                 (anti-rollback)
 *   3. CRC32 + SHA-256 del payload contra el manifiesto
 *   4. firma Ed25519                                       (la más cara, última)
 *
 * Los bounds van primero porque SOLO RECHAZAN: evitan un desbordamiento de
 * buffer con una imagen manipulada. No aceptan nada en función de datos sin
 * verificar, así que no pueden abrir un agujero. La firma va al final porque
 * verificar Ed25519 cuesta decenas de miles de operaciones y no tiene
 * sentido pagarlas con basura de red.
 *
 * Lógica pura (solo <stdint.h>) para testearse nativamente con g++ en
 * tests/native/ - sin hardware y sin criptografía real.
 */

#ifndef FW_IMAGE_H
#define FW_IMAGE_H

#include <stdint.h>
#include <stddef.h>

/** Tamaño fijo del manifiesto */
#define FW_MAGIC          0x4D495746u  /* "FWIM" little-endian */
#define FW_HEADER_LEN     64u
#define FW_SIGNATURE_LEN  64u          /* Ed25519 */

/** Códigos de rechazo. Cada uno es una amenaza concreta bloqueada. */
enum class FwReject : uint8_t {
    NONE = 0,
    TOO_SMALL,          // imagen menor que un manifiesto
    BAD_MAGIC,          // no es una imagen nuestra
    BAD_HEADER_LEN,     // manifiesto de tamaño inesperado
    PAYLOAD_TOO_BIG,    // no cabe en el slot de flash
    IMAGE_TRUNCATED,    // faltan bytes para payload+firma
    IMAGE_TOO_BIG,      // overflow del tamaño total
    VERSION_NOT_NEWER,  // downgrade o re-instalación de la misma versión
    HEADER_CRC_FAIL,    // manifiesto corrupto
    PAYLOAD_CRC_FAIL,   // payload corrupto
    PAYLOAD_HASH_FAIL,  // payload no coincide con el manifiesto
    NO_CRYPTO_BACKEND,  // sin backend: fail-closed
    SIGNATURE_INVALID,  // firma Ed25519 no válida
};

/** Resultado de intentar cargar y verificar una imagen */
struct FwVerdict {
    FwReject reject = FwReject::NONE;
    uint32_t version = 0;
    uint32_t payload_len = 0;
    uint32_t build_id = 0;
    uint32_t total_len = 0;      // header + payload + firma
    uint32_t steps_done = 0;     // hasta dónde llegó (para diagnóstico)

    bool ok() const { return reject == FwReject::NONE; }
};

/**
 * Interfaz de la criptografía. La implementación real (Ed25519) NO está
 * incluida: se usa una librería auditada externa fijada por hash de commit.
 *
 * Existe la interfaz para que el pipeline sea testeable nativamente y para
 * que quede explícito dónde entra la dependencia criptográfica.
 *
 * IMPORTANTE: mientras no haya implementación real, toda imagen debe ser
 * RECHAZADA. El comportamiento por defecto (fail-closed) es intencionado.
 */
class CryptoBackend {
public:
    virtual ~CryptoBackend() {}

    /**
     * Clave pública de Ed25519 con la que se verifica. Va compilada dentro
     * del firmware y por eso una actualización NO puede cambiarla: solo el
     * bootloader o ST-Link pueden, y ambos son físicos. Si el firmware
     * pudiera cambiar su propia clave pública, la firma no protegería nada.
     * @return nullptr si no hay clave (todo se rechaza: fail-closed)
     */
    virtual const uint8_t* publicKey() const = 0;

    /** SHA-256 de `len` bytes. @return false si el backend no puede hacerlo. */
    virtual bool sha256(const uint8_t* data, size_t len, uint8_t out[32]) = 0;

    /**
     * Verifica una firma Ed25519 sobre `msg`.
     * @return true solo si la firma es válida.
     */
    virtual bool ed25519_verify(const uint8_t pubkey[32],
                                const uint8_t* msg, size_t msg_len,
                                const uint8_t sig[FW_SIGNATURE_LEN]) = 0;
};

/**
 * Manifiesto de una imagen de firmware. Se rellena leyendo el buffer, sin
 * reservar memoria ni copiar el payload.
 */
struct FwManifest {
    uint32_t magic = 0;
    uint32_t header_len = 0;
    uint32_t version = 0;
    uint32_t payload_len = 0;
    uint32_t build_id = 0;
    uint32_t payload_crc32 = 0;
    uint8_t  payload_sha256[32] = {0};
    uint32_t header_crc32 = 0;
    uint32_t reserved = 0;
};

/** Lectura del manifiesto. No verifica nada: solo deserializa los campos. */
FwManifest fwParseManifest(const uint8_t* image, size_t image_len);

/** CRC32 estándar (IEEE 802.3), misma polinomio que zlib/p7zip. */
uint32_t fwCrc32(const uint8_t* data, size_t len);

/** Longitud total que declara la imagen (header + payload + firma). */
uint32_t fwTotalLength(const FwManifest& m);

/**
 * Verifica una imagen completa.
 *
 * @param image      buffer de la imagen (manifiesto + payload + firma)
 * @param image_len  bytes disponibles en el buffer
 * @param slot_max   tamaño máximo del slot de flash donde se escribiría
 * @param current_version versión ya instalada (para anti-rollback)
 * @param crypto     backend criptográfico; nullptr => todo se rechaza
 *
 * @return veredicto con el motivo del rechazo si algo falla
 */
FwVerdict fwVerifyImage(const uint8_t* image, size_t image_len,
                        uint32_t slot_max, uint32_t current_version,
                        CryptoBackend* crypto);

/** Texto legible del motivo de rechazo (telemetría/diagnóstico). */
const char* fwRejectText(FwReject r);

#endif // FW_IMAGE_H
