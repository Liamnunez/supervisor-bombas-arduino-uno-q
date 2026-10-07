/**
 * @file fw_image.cpp
 * @brief Verificación de imagen de firmware
 */

#include "fw_image.h"

namespace {

// Lectura little-endian explícita: el MCU es little-endian pero no vamos a
// depender de eso para el formato, que también se genera en Linux.
inline uint32_t rd32(const uint8_t* p) {
    return static_cast<uint32_t>(p[0]) |
           (static_cast<uint32_t>(p[1]) << 8) |
           (static_cast<uint32_t>(p[2]) << 16) |
           (static_cast<uint32_t>(p[3]) << 24);
}

// Comparación en tiempo constante. La longitud de la clave pública es fija y
// pública, así que compararbyte a byte con acumulador no filtra información
// por tiempo, pero el acumulador evita que el compilador saque el bucle.
inline bool ct_equal(const uint8_t* a, const uint8_t* b, size_t len) {
    uint8_t diff = 0;
    for (size_t i = 0; i < len; i++) diff = static_cast<uint8_t>(diff | (a[i] ^ b[i]));
    return diff == 0;
}

constexpr uint32_t CRC32_POLY = 0xEDB88320u;

}  // namespace

uint32_t fwCrc32(const uint8_t* data, size_t len) {
    uint32_t crc = 0xFFFFFFFFu;
    for (size_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 1u) ? ((crc >> 1) ^ CRC32_POLY) : (crc >> 1);
        }
    }
    return ~crc;
}

FwManifest fwParseManifest(const uint8_t* image, size_t image_len) {
    FwManifest m;
    if (image == nullptr || image_len < FW_HEADER_LEN) return m;

    m.magic = rd32(image + 0x00);
    m.header_len = rd32(image + 0x04);
    m.version = rd32(image + 0x08);
    m.payload_len = rd32(image + 0x0C);
    m.build_id = rd32(image + 0x10);
    m.payload_crc32 = rd32(image + 0x14);
    for (int i = 0; i < 32; i++) m.payload_sha256[i] = image[0x18 + i];
    m.header_crc32 = rd32(image + 0x38);
    m.reserved = rd32(image + 0x3C);
    return m;
}

uint32_t fwTotalLength(const FwManifest& m) {
    return m.header_len + m.payload_len + FW_SIGNATURE_LEN;
}

FwVerdict fwVerifyImage(const uint8_t* image, size_t image_len,
                        uint32_t slot_max, uint32_t current_version,
                        CryptoBackend* crypto) {
    FwVerdict v;

    // --- 1. Bounds mínimos: ¿hay siquiera un manifiesto completo? ---
    if (image == nullptr || image_len < FW_HEADER_LEN) {
        v.reject = FwReject::TOO_SMALL;
        return v;
    }
    v.steps_done = 1;

    FwManifest m = fwParseManifest(image, image_len);
    v.version = m.version;
    v.payload_len = m.payload_len;
    v.build_id = m.build_id;

    // --- 2. Identidad de la imagen ---
    if (m.magic != FW_MAGIC) {
        v.reject = FwReject::BAD_MAGIC;
        return v;
    }
    if (m.header_len != FW_HEADER_LEN) {
        v.reject = FwReject::BAD_HEADER_LEN;
        return v;
    }
    v.steps_done = 2;

    // --- 3. ¿El payload cabe en el slot? (bounds: solo rechaza) ---
    if (m.payload_len == 0 || m.payload_len > slot_max) {
        v.reject = FwReject::PAYLOAD_TOO_BIG;
        return v;
    }
    v.steps_done = 3;

    // --- 4. Overflow y truncamiento ---
    // payload_len ya está acotado por slot_max, así que la suma no puede
    // desbordar; aun así se comprueba el orden para no confiar en el índice.
    const uint64_t total = static_cast<uint64_t>(m.header_len) + m.payload_len + FW_SIGNATURE_LEN;
    if (total > 0xFFFFFFFFull) {
        v.reject = FwReject::IMAGE_TOO_BIG;
        return v;
    }
    if (total > image_len) {
        v.reject = FwReject::IMAGE_TRUNCATED;
        return v;
    }
    v.total_len = static_cast<uint32_t>(total);
    v.steps_done = 4;

    // --- 5. Anti-rollback: la versión debe ser estrictamente mayor ---
    // Comparación normal, no con signo: uint32 completo, 0xFFFFFFFF es legal.
    if (m.version <= current_version) {
        v.reject = FwReject::VERSION_NOT_NEWER;
        return v;
    }
    v.steps_done = 5;

    // --- 6. CRC del manifiesto ---
    // header_crc32 cubre 0x00..0x37, es decir los campos que vienen antes.
    if (fwCrc32(image, 0x38) != m.header_crc32) {
        v.reject = FwReject::HEADER_CRC_FAIL;
        return v;
    }
    v.steps_done = 6;

    const uint8_t* payload = image + m.header_len;
    const uint8_t* sig = image + m.header_len + m.payload_len;

    // --- 7. CRC y SHA-256 del payload ---
    if (fwCrc32(payload, m.payload_len) != m.payload_crc32) {
        v.reject = FwReject::PAYLOAD_CRC_FAIL;
        return v;
    }
    v.steps_done = 7;

    // --- 8. Firma Ed25519 (la más cara, la última) ---
    // Fail-closed: sin backend no se acepta nada.
    if (crypto == nullptr) {
        v.reject = FwReject::NO_CRYPTO_BACKEND;
        return v;
    }

    uint8_t digest[32];
    if (!crypto->sha256(payload, m.payload_len, digest)) {
        v.reject = FwReject::NO_CRYPTO_BACKEND;
        return v;
    }
    if (!ct_equal(digest, m.payload_sha256, 32)) {
        v.reject = FwReject::PAYLOAD_HASH_FAIL;
        return v;
    }
    v.steps_done = 8;

    // La firma cubre manifiesto + payload, excluyendo el propio CRC del
    // manifiesto y la propia firma (si no, sería circular).
    // El payload_sha256 ya está firmado, así que verificarlo antes de la
    // firma solo evita trabajo, no salta ninguna comprobación de seguridad.
    const uint8_t* pubkey = crypto->publicKey();
    if (pubkey == nullptr ||
        !crypto->ed25519_verify(pubkey, image, m.header_len + m.payload_len, sig)) {
        v.reject = FwReject::SIGNATURE_INVALID;
        return v;
    }
    v.steps_done = 9;

    v.reject = FwReject::NONE;
    return v;
}

const char* fwRejectText(FwReject r) {
    switch (r) {
        case FwReject::NONE:              return "OK";
        case FwReject::TOO_SMALL:         return "imagen menor que el manifiesto";
        case FwReject::BAD_MAGIC:         return "magic invalido";
        case FwReject::BAD_HEADER_LEN:    return "tamano de manifiesto inesperado";
        case FwReject::PAYLOAD_TOO_BIG:   return "payload no cabe en el slot";
        case FwReject::IMAGE_TRUNCATED:   return "imagen truncada";
        case FwReject::IMAGE_TOO_BIG:     return "tamano total desbordado";
        case FwReject::VERSION_NOT_NEWER: return "version no mas nueva";
        case FwReject::HEADER_CRC_FAIL:   return "CRC de manifiesto incorrecto";
        case FwReject::PAYLOAD_CRC_FAIL:  return "CRC de payload incorrecto";
        case FwReject::PAYLOAD_HASH_FAIL: return "SHA-256 de payload incorrecto";
        case FwReject::NO_CRYPTO_BACKEND: return "sin backend criptografico";
        case FwReject::SIGNATURE_INVALID: return "firma invalida";
        default:                          return "error desconocido";
    }
}
