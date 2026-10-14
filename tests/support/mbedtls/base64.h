#pragma once
// Host stand-in for mbedtls_base64_encode, with the same contract: NUL-terminated output, and
// MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL (with the needed size in *olen) when dlen cannot hold it.
#include <cstddef>
#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A
inline int mbedtls_base64_encode(unsigned char* dst, size_t dlen, size_t* olen, const unsigned char* src, size_t slen) {
    static const char table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t need = (slen + 2) / 3 * 4 + 1;
    if (dlen < need) { *olen = need; return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL; }
    size_t o = 0;
    for (size_t i = 0; i < slen; i += 3) {
        const unsigned v = (unsigned)src[i] << 16 | (i + 1 < slen ? (unsigned)src[i + 1] << 8 : 0) | (i + 2 < slen ? src[i + 2] : 0);
        dst[o++] = table[v >> 18 & 63]; dst[o++] = table[v >> 12 & 63];
        dst[o++] = i + 1 < slen ? table[v >> 6 & 63] : '=';
        dst[o++] = i + 2 < slen ? table[v & 63] : '=';
    }
    dst[o] = 0;
    *olen = o;
    return 0;
}
