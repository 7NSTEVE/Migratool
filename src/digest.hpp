#pragma once

#include <windows.h>
#include <bcrypt.h>
#include <stdexcept>
#include <string>

namespace mt {
class Digest {
    BCRYPT_ALG_HANDLE algorithm_{};
    BCRYPT_HASH_HANDLE hash_{};
public:
    Digest() {
        if (BCryptOpenAlgorithmProvider(&algorithm_, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0)
            throw std::runtime_error("No se pudo abrir SHA-256");
        if (BCryptCreateHash(algorithm_, &hash_, nullptr, 0, nullptr, 0, 0) < 0) {
            BCryptCloseAlgorithmProvider(algorithm_, 0);
            throw std::runtime_error("No se pudo inicializar SHA-256");
        }
    }
    Digest(const Digest&) = delete;
    ~Digest() {
        BCryptDestroyHash(hash_);
        BCryptCloseAlgorithmProvider(algorithm_, 0);
    }
    void add(const char* data, size_t size) {
        if (size > ULONG_MAX || BCryptHashData(hash_, reinterpret_cast<PUCHAR>(const_cast<char*>(data)),
                                               static_cast<ULONG>(size), 0) < 0)
            throw std::runtime_error("No se pudo calcular SHA-256");
    }
    std::string finish() {
        unsigned char bytes[32];
        if (BCryptFinishHash(hash_, bytes, sizeof(bytes), 0) < 0)
            throw std::runtime_error("SHA-256 falló");
        static constexpr char hex[] = "0123456789abcdef";
        std::string result;
        result.reserve(64);
        for (const auto byte : bytes) {
            result += hex[byte >> 4];
            result += hex[byte & 15];
        }
        return result;
    }
};
}
