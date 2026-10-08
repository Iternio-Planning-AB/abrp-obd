#pragma once
#include <vector>
#include <string>
std::vector<std::string> upload_hash_inputs;
bool upload_hash_failure = false;
inline int mbedtls_sha256_ret(const unsigned char* input, size_t length, unsigned char output[32], int is224) {
    check(is224 == 0, "SHA256 not SHA224");
    upload_hash_inputs.emplace_back(reinterpret_cast<const char*>(input), length);
    if (upload_hash_failure) return -1;
    const std::string hex = "0123456789abcdef";
    std::string encoded(length * 2, '0');
    for (size_t i = 0; i < length; ++i) {
        encoded[2 * i] = hex[input[i] >> 4];
        encoded[2 * i + 1] = hex[input[i] & 15];
    }
    std::cout << "SHA256 " << encoded << std::endl;
    std::string digest;
    check(static_cast<bool>(std::cin >> digest) && digest.size() == 64, "Python hashlib SHA256 reply");
    for (size_t i = 0; i < 32; ++i) {
        const auto high = hex.find(digest[2 * i]), low = hex.find(digest[2 * i + 1]);
        check(high < 16 && low < 16, "SHA256 digest hex");
        output[i] = (high << 4) | low;
    }
    return 0;
}
