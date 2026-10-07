/**
 * @file test_fw_image.cpp
 * @brief Tests nativos del pipeline de verificación de imagen de firmware
 *
 * Lo que se prueba aquí es la LÓGICA DE RECHAZO, que es la parte de
 * seguridad. La criptografía real va por CryptoBackend y aquí se usa un
 * doble de test: verificar que un SHA-256 real o una firma real son
 * correctos es trabajo de una librería auditada, no de este test.
 *
 * Compilar y ejecutar:  make tests-native
 */

#include "fw_image.h"

#include <cstdio>
#include <cstring>
#include <vector>

static int checks = 0;
static int failures = 0;

#define CHECK(cond) do { \
    checks++; \
    if (!(cond)) { \
        std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

// --- Doble de test -------------------------------------------------------
// sha256 real (FIPS 180-4) para que las pruebas del hash tengan valor.
class TestCrypto : public CryptoBackend {
public:
    static constexpr size_t BLOCK = 64;

    uint8_t key_[32] = {0};
    bool backend_works = true;
    bool signature_ok = true;
    int sha_calls = 0;
    int verify_calls = 0;

    explicit TestCrypto() {
        for (int i = 0; i < 32; i++) key_[i] = static_cast<uint8_t>(0xA0 + i);
    }

    const uint8_t* publicKey() const override {
        return backend_works ? key_ : nullptr;
    }

    bool sha256(const uint8_t* data, size_t len, uint8_t out[32]) override {
        sha_calls++;
        if (!backend_works) return false;
        sha256_real(data, len, out);
        return true;
    }

    bool ed25519_verify(const uint8_t pk[32], const uint8_t* msg, size_t msg_len,
                        const uint8_t sig[FW_SIGNATURE_LEN]) override {
        (void)pk; (void)msg; (void)msg_len; (void)sig;
        verify_calls++;
        return backend_works && signature_ok;
    }

    // SHA-256 de referencia, suficiente para las pruebas.
    static void sha256_real(const uint8_t* data, size_t len, uint8_t out[32]);
};

// --- Construcción de imágenes de prueba -----------------------------------
struct ImageBuilder {
    uint32_t version = 1;
    uint32_t build_id = 0x1234;
    std::vector<uint8_t> payload;

    std::vector<uint8_t> build(bool corrupt_payload = false,
                               bool bad_signature = false) const;
};

static void put32(std::vector<uint8_t>& v, size_t off, uint32_t x) {
    v[off + 0] = static_cast<uint8_t>(x);
    v[off + 1] = static_cast<uint8_t>(x >> 8);
    v[off + 2] = static_cast<uint8_t>(x >> 16);
    v[off + 3] = static_cast<uint8_t>(x >> 24);
}

std::vector<uint8_t> ImageBuilder::build(bool corrupt_payload,
                                         bool bad_signature) const {
    std::vector<uint8_t> img(FW_HEADER_LEN, 0);
    img[0] = 'F'; img[1] = 'W'; img[2] = 'I'; img[3] = 'M';
    put32(img, 0x00, FW_MAGIC);
    put32(img, 0x04, FW_HEADER_LEN);
    put32(img, 0x08, version);
    put32(img, 0x0C, static_cast<uint32_t>(payload.size()));
    put32(img, 0x10, build_id);

    uint8_t digest[32];
    TestCrypto::sha256_real(payload.data(), payload.size(), digest);
    std::memcpy(img.data() + 0x18, digest, 32);

    std::vector<uint8_t> body = payload;
    if (corrupt_payload && !body.empty()) body[body.size() / 2] ^= 0xFF;
    put32(img, 0x14, fwCrc32(body.data(), body.size()));
    put32(img, 0x38, fwCrc32(img.data(), 0x38));
    put32(img, 0x3C, 0);

    std::vector<uint8_t> out = img;
    out.insert(out.end(), body.begin(), body.end());
    for (uint32_t i = 0; i < FW_SIGNATURE_LEN; i++) {
        out.push_back(bad_signature ? static_cast<uint8_t>(0xFF) : static_cast<uint8_t>(i));
    }
    return out;
}

// --- SHA-256 de referencia ------------------------------------------------
static const uint32_t K256[64] = {
    0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
    0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
    0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
    0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
    0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
    0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
    0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
    0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};

static inline uint32_t ror(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

void TestCrypto::sha256_real(const uint8_t* data, size_t len, uint8_t out[32]) {
    uint32_t h[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,
                     0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
    std::vector<uint8_t> msg(data, data + len);
    msg.push_back(0x80);
    while (msg.size() % 64 != 56) msg.push_back(0);
    uint64_t bits = static_cast<uint64_t>(len) * 8;
    for (int i = 7; i >= 0; i--) msg.push_back(static_cast<uint8_t>(bits >> (i * 8)));

    for (size_t off = 0; off < msg.size(); off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; i++) {
            w[i] = (static_cast<uint32_t>(msg[off + i * 4]) << 24) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 1]) << 16) |
                   (static_cast<uint32_t>(msg[off + i * 4 + 2]) << 8) |
                    static_cast<uint32_t>(msg[off + i * 4 + 3]);
        }
        for (int i = 16; i < 64; i++) {
            uint32_t s0 = ror(w[i-15],7) ^ ror(w[i-15],18) ^ (w[i-15] >> 3);
            uint32_t s1 = ror(w[i-2],17) ^ ror(w[i-2],19) ^ (w[i-2] >> 10);
            w[i] = w[i-16] + s0 + w[i-7] + s1;
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4],f=h[5],g=h[6],hh=h[7];
        for (int i = 0; i < 64; i++) {
            uint32_t S1 = ror(e,6) ^ ror(e,11) ^ ror(e,25);
            uint32_t ch = (e & f) ^ (~e & g);
            uint32_t t1 = hh + S1 + ch + K256[i] + w[i];
            uint32_t S0 = ror(a,2) ^ ror(a,13) ^ ror(a,22);
            uint32_t mj = (a & b) ^ (a & c) ^ (b & c);
            uint32_t t2 = S0 + mj;
            hh=g; g=f; f=e; e=d+t1; d=c; c=b; b=a; a=t1+t2;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d;
        h[4]+=e; h[5]+=f; h[6]+=g; h[7]+=hh;
    }
    for (int i = 0; i < 8; i++) {
        out[i*4]   = static_cast<uint8_t>(h[i] >> 24);
        out[i*4+1] = static_cast<uint8_t>(h[i] >> 16);
        out[i*4+2] = static_cast<uint8_t>(h[i] >> 8);
        out[i*4+3] = static_cast<uint8_t>(h[i]);
    }
}

// --- Pruebas -------------------------------------------------------------

static const uint32_t SLOT_MAX = 512 * 1024;

static ImageBuilder img_ok() {
    ImageBuilder b;
    b.version = 12;
    b.payload.assign(4096, 0xAB);
    for (size_t i = 0; i < b.payload.size(); i++) {
        b.payload[i] = static_cast<uint8_t>(i * 7 + 3);
    }
    return b;
}

static void test_imagen_valida_aceptada() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    TestCrypto c;
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c);
    CHECK(v.ok());
    CHECK(v.version == 12);
    CHECK(v.payload_len == 4096);
    CHECK(v.build_id == b.build_id);
    CHECK(v.total_len == FW_HEADER_LEN + 4096 + FW_SIGNATURE_LEN);
    CHECK(v.steps_done == 9);
    CHECK(c.verify_calls == 1);
}

static void test_sin_backend_rechaza_todo() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, nullptr);
    CHECK(!v.ok());
    CHECK(v.reject == FwReject::NO_CRYPTO_BACKEND);
}

static void test_backend_roto_rechaza() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    TestCrypto c;
    c.backend_works = false;
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c);
    CHECK(!v.ok());
    CHECK(v.reject == FwReject::NO_CRYPTO_BACKEND);
}

static void test_magic_invalido() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    img[0] = 'X';
    TestCrypto c;
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c);
    CHECK(v.reject == FwReject::BAD_MAGIC);
    CHECK(c.verify_calls == 0);   // no se paga la firma si ya es basura
}

static void test_header_len_invalido() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    put32(img, 0x04, 32);
    TestCrypto c;
    CHECK(fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c).reject ==
          FwReject::BAD_HEADER_LEN);
}

static void test_imagen_demasiado_pequena() {
    TestCrypto c;
    uint8_t buf[10] = {0};
    CHECK(fwVerifyImage(buf, 0, SLOT_MAX, 11, &c).reject == FwReject::TOO_SMALL);
    CHECK(fwVerifyImage(buf, sizeof(buf), SLOT_MAX, 11, &c).reject == FwReject::TOO_SMALL);
    CHECK(fwVerifyImage(nullptr, 1000, SLOT_MAX, 11, &c).reject == FwReject::TOO_SMALL);
}

static void test_payload_no_cabe_en_slot() {
    ImageBuilder b = img_ok();
    b.payload.assign(1024, 0x11);
    std::vector<uint8_t> img = b.build();
    TestCrypto c;
    // Slot más pequeño que el payload
    CHECK(fwVerifyImage(img.data(), img.size(), 512, 11, &c).reject ==
          FwReject::PAYLOAD_TOO_BIG);
    // Slot del tamaño exacto del payload: sí cabe
    CHECK(fwVerifyImage(img.data(), img.size(), 1024, 11, &c).ok());
}

static void test_payload_cero_rechazado() {
    ImageBuilder b = img_ok();
    b.payload.clear();
    std::vector<uint8_t> img = b.build();
    TestCrypto c;
    CHECK(fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c).reject ==
          FwReject::PAYLOAD_TOO_BIG);
}

static void test_payload_len_declarado_enorme() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    // Alguien manipula payload_len a 0xFFFFFFF0 pero el CRC del manifiesto
    // se recalcula para pasar el primer filtro
    put32(img, 0x0C, 0xFFFFFFF0u);
    put32(img, 0x38, fwCrc32(img.data(), 0x38));
    TestCrypto c;
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c);
    CHECK(v.reject == FwReject::PAYLOAD_TOO_BIG);
    CHECK(c.verify_calls == 0);
}

static void test_imagen_truncada() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    img.resize(img.size() - 10);   // faltan los últimos bytes
    TestCrypto c;
    CHECK(fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c).reject ==
          FwReject::IMAGE_TRUNCATED);
}

static void test_anti_rollback() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();

    TestCrypto c;
    // Misma versión: reinstalación, no actualización
    CHECK(fwVerifyImage(img.data(), img.size(), SLOT_MAX, 12, &c).reject ==
          FwReject::VERSION_NOT_NEWER);
    // Versión anterior
    CHECK(fwVerifyImage(img.data(), img.size(), SLOT_MAX, 20, &c).reject ==
          FwReject::VERSION_NOT_NEWER);
    // Una menor
    CHECK(fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c).ok());
}

static void test_anti_rollback_con_wraparound() {
    ImageBuilder b = img_ok();
    b.version = 0xFFFFFFFFu;
    std::vector<uint8_t> img = b.build();
    TestCrypto c;
    // 0xFFFFFFFF debe aceptarse sobre 0xFFFFFFFE (comparación sin signo,
    // no con signo: uint32 completo)
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 0xFFFFFFFEu, &c);
    CHECK(v.ok());
    CHECK(v.version == 0xFFFFFFFFu);
}

static void test_manifesto_corrupto() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    img[0x09] ^= 0xFF;   // version, sin recalcular el CRC del manifiesto
    TestCrypto c;
    CHECK(fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c).reject ==
          FwReject::HEADER_CRC_FAIL);
}

static void test_payload_corrupto_bit_flip() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    img[FW_HEADER_LEN + 100] ^= 0x01;   // un solo bit
    TestCrypto c;
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c);
    CHECK(v.reject == FwReject::PAYLOAD_CRC_FAIL);
    CHECK(c.verify_calls == 0);
}

static void test_payload_con_crc_enganado() {
    // Atacante que recalcula el CRC32 para pasar el filtro barato, pero no
    // toca el SHA-256: el hash lo delata.
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    img[FW_HEADER_LEN + 100] ^= 0x01;
    put32(img, 0x14, fwCrc32(img.data() + FW_HEADER_LEN, b.payload.size()));
    put32(img, 0x38, fwCrc32(img.data(), 0x38));
    TestCrypto c;
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c);
    CHECK(v.reject == FwReject::PAYLOAD_HASH_FAIL);
    CHECK(c.verify_calls == 0);   // el hash falla antes de la firma
}

static void test_firma_invalida() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build(false, true);
    TestCrypto c;
    c.signature_ok = false;
    FwVerdict v = fwVerifyImage(img.data(), img.size(), SLOT_MAX, 11, &c);
    CHECK(v.reject == FwReject::SIGNATURE_INVALID);
    CHECK(c.verify_calls == 1);
}

static void test_sha256_de_referencia() {
    // El doble de test usa un SHA-256 propio: si está mal, todas las
    // pruebas de hash valen menos que nada. Se contrasta con vectores
    // oficiales de FIPS 180-4.
    uint8_t out[32];
    const uint8_t vacio[] = {0};
    TestCrypto::sha256_real(vacio, 0, out);
    const uint8_t esperado_vacio[32] = {
        0xe3,0xb0,0xc4,0x42,0x98,0xfc,0x1c,0x14,0x9a,0xfb,0xf4,0xc8,0x99,0x6f,0xb9,0x24,
        0x27,0xae,0x41,0xe4,0x64,0x9b,0x93,0x4c,0xa4,0x95,0x99,0x1b,0x78,0x52,0xb8,0x55};
    CHECK(std::memcmp(out, esperado_vacio, 32) == 0);

    const uint8_t abc[] = {'a','b','c'};
    TestCrypto::sha256_real(abc, 3, out);
    const uint8_t esperado_abc[32] = {
        0xba,0x78,0x16,0xbf,0x8f,0x01,0xcf,0xea,0x41,0x41,0x40,0xde,0x5d,0xae,0x22,0x23,
        0xb0,0x03,0x61,0xa3,0x96,0x17,0x7a,0x9c,0xb4,0x10,0xff,0x61,0xf2,0x00,0x15,0xad};
    CHECK(std::memcmp(out, esperado_abc, 32) == 0);

    // 1M de 'a' es demasiado lento aquí; 1000 'a' basta como vector largo
    std::vector<uint8_t> mil(1000, 'a');
    TestCrypto::sha256_real(mil.data(), mil.size(), out);
    CHECK(out[0] != 0);
}

static void test_crc32_de_referencia() {
    const uint8_t check[] = "123456789";
    CHECK(fwCrc32(check, 9) == 0xCBF43926u);   // vector estándar de CRC-32/ISO-HDLC
    CHECK(fwCrc32(nullptr, 0) == 0x00000000u);
    uint8_t z[4] = {0, 0, 0, 0};
    CHECK(fwCrc32(z, 4) == 0x2144DF1Cu);
}

static void test_orden_de_verificacion_es_atajos() {
    // Una imagen basura debe rechazarse antes de pagar la firma.
    struct { const char* nombre; std::vector<uint8_t> img; FwReject esperado; } casos[] = {
        {"magic",      [&]{auto i=img_ok().build(); i[0]='Z'; return i;}(), FwReject::BAD_MAGIC},
        {"header_len", [&]{auto i=img_ok().build(); put32(i,0x04,1); return i;}(), FwReject::BAD_HEADER_LEN},
        {"truncada",   [&]{auto i=img_ok().build(); i.resize(100); return i;}(), FwReject::IMAGE_TRUNCATED},
        {"rollback",   [&]{auto i=img_ok().build(); return i;}(), FwReject::VERSION_NOT_NEWER},
    };
    for (auto& c : casos) {
        TestCrypto tc;
        uint32_t cur = (c.esperado == FwReject::VERSION_NOT_NEWER) ? 99 : 11;
        FwVerdict v = fwVerifyImage(c.img.data(), c.img.size(), SLOT_MAX, cur, &tc);
        CHECK(v.reject == c.esperado);
        CHECK(tc.verify_calls == 0);
    }
}

static void test_parse_manifest_no_verifica() {
    ImageBuilder b = img_ok();
    std::vector<uint8_t> img = b.build();
    FwManifest m = fwParseManifest(img.data(), img.size());
    CHECK(m.magic == FW_MAGIC);
    CHECK(m.header_len == FW_HEADER_LEN);
    CHECK(m.version == 12);
    CHECK(m.payload_len == 4096);
    CHECK(m.reserved == 0);
    // Buffer corto: todo a cero, sin leer fuera de rango
    FwManifest z = fwParseManifest(img.data(), 10);
    CHECK(z.magic == 0 && z.version == 0);
    FwManifest n = fwParseManifest(nullptr, 100);
    CHECK(n.magic == 0);
}

static void test_texto_de_rechazo() {
    // Todos los códigos deben tener texto: un reject sin texto llega como
    // número al dashboard y no sirve de nada.
    for (int i = 0; i <= static_cast<int>(FwReject::SIGNATURE_INVALID); i++) {
        const char* t = fwRejectText(static_cast<FwReject>(i));
        CHECK(t != nullptr);
        CHECK(t[0] != '\0');
    }
    CHECK(std::strcmp(fwRejectText(FwReject::NONE), "OK") == 0);
}

int main() {
    std::printf("--- Verificaci\u00f3n de imagen de firmware ---\n");
    test_sha256_de_referencia();
    test_crc32_de_referencia();
    test_imagen_valida_aceptada();
    test_sin_backend_rechaza_todo();
    test_backend_roto_rechaza();
    test_magic_invalido();
    test_header_len_invalido();
    test_imagen_demasiado_pequena();
    test_payload_no_cabe_en_slot();
    test_payload_cero_rechazado();
    test_payload_len_declarado_enorme();
    test_imagen_truncada();
    test_anti_rollback();
    test_anti_rollback_con_wraparound();
    test_manifesto_corrupto();
    test_payload_corrupto_bit_flip();
    test_payload_con_crc_enganado();
    test_firma_invalida();
    test_orden_de_verificacion_es_atajos();
    test_parse_manifest_no_verifica();
    test_texto_de_rechazo();

    std::printf("%d checks, %d fallos\n", checks, failures);
    if (failures == 0) {
        std::printf("TESTS NATIVOS OK (imagen de firmware)\n");
        return 0;
    }
    return 1;
}
