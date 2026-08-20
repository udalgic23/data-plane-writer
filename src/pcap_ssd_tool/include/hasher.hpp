#pragma once
#include <array>
#include <cstdint>
#include <openssl/evp.h>

constexpr size_t DIGEST_SIZE = 32;

class Hasher {
public:
    Hasher();
    ~Hasher();
    void update(const void *data, size_t len);
    std::array<uint8_t, DIGEST_SIZE> final();

private:
    EVP_MD_CTX *ctx_;
};

bool digests_equal(const std::array<uint8_t, DIGEST_SIZE> &a, const std::array<uint8_t, DIGEST_SIZE> &b);