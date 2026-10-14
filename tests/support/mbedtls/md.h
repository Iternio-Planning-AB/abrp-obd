#pragma once
// Host stand-in for the mbedtls_md_* calls the firmware makes: SHA-256 and HMAC-SHA256 only.
#include "sha256_impl.h"
typedef enum { MBEDTLS_MD_NONE = 0, MBEDTLS_MD_SHA256 = 6 } mbedtls_md_type_t;
struct mbedtls_md_info_t { mbedtls_md_type_t type; };
inline int host_md_fail = 0; // while set, every call fails
inline const mbedtls_md_info_t* mbedtls_md_info_from_type(mbedtls_md_type_t type) {
    static const mbedtls_md_info_t sha256{MBEDTLS_MD_SHA256};
    return type == MBEDTLS_MD_SHA256 ? &sha256 : nullptr;
}
inline int mbedtls_md(const mbedtls_md_info_t* info, const unsigned char* input, size_t ilen, unsigned char* output) {
    if (!info || host_md_fail) return -1;
    host_sha256::digest(input, ilen, output);
    return 0;
}
inline int mbedtls_md_hmac(const mbedtls_md_info_t* info, const unsigned char* key, size_t keylen,
                           const unsigned char* input, size_t ilen, unsigned char* output) {
    if (!info || host_md_fail) return -1;
    unsigned char block[64] = {}, inner_key[64], outer_key[64], inner[32];
    if (keylen > 64) host_sha256::digest(key, keylen, block); else std::memcpy(block, key, keylen);
    for (int i = 0; i < 64; ++i) { inner_key[i] = block[i] ^ 0x36; outer_key[i] = block[i] ^ 0x5c; }
    unsigned char* msg = new unsigned char[64 + (ilen > 32 ? ilen : 32)];
    std::memcpy(msg, inner_key, 64); std::memcpy(msg + 64, input, ilen);
    host_sha256::digest(msg, 64 + ilen, inner);
    std::memcpy(msg, outer_key, 64); std::memcpy(msg + 64, inner, 32);
    host_sha256::digest(msg, 96, output);
    delete[] msg;
    return 0;
}
