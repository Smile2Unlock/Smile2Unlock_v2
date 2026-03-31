//
// Created by ation_ciger on 2026/3/19.
// AES-256-CBC implementation backed by mbed TLS
//
module;

#include <mbedtls/aes.h>
#include <mbedtls/ctr_drbg.h>
#include <mbedtls/entropy.h>

export module crypto;

import std;

export constexpr size_t kAesKeySize = 32;
export constexpr size_t kAesBlockSize = 16;
export constexpr size_t kAesIvSize = 16;

namespace {

bool IsHexDigit(const char ch) {
    return std::isxdigit(static_cast<unsigned char>(ch)) != 0;
}

uint8_t HexNibble(const char ch) {
    if (ch >= '0' && ch <= '9') {
        return static_cast<uint8_t>(ch - '0');
    }
    if (ch >= 'a' && ch <= 'f') {
        return static_cast<uint8_t>(10 + ch - 'a');
    }
    if (ch >= 'A' && ch <= 'F') {
        return static_cast<uint8_t>(10 + ch - 'A');
    }
    throw std::runtime_error("Invalid hex character");
}

void ValidateHex(std::string_view hex, const size_t expected_bytes) {
    if (hex.size() != expected_bytes * 2) {
        throw std::runtime_error("Invalid hex length");
    }
    for (const char ch : hex) {
        if (!IsHexDigit(ch)) {
            throw std::runtime_error("Invalid hex character");
        }
    }
}

std::vector<uint8_t> AddPadding(const uint8_t* data, const size_t len) {
    const size_t pad_len = kAesBlockSize - (len % kAesBlockSize);
    std::vector<uint8_t> result(len + pad_len);
    if (len > 0) {
        std::memcpy(result.data(), data, len);
    }
    std::memset(result.data() + len, static_cast<int>(pad_len), pad_len);
    return result;
}

std::vector<uint8_t> RemovePadding(const uint8_t* data, const size_t len) {
    if (len == 0 || len % kAesBlockSize != 0) {
        throw std::runtime_error("Invalid padded data length");
    }

    const uint8_t pad_len = data[len - 1];
    if (pad_len == 0 || pad_len > kAesBlockSize || len < pad_len) {
        throw std::runtime_error("Invalid padding");
    }

    for (size_t i = len - pad_len; i < len; ++i) {
        if (data[i] != pad_len) {
            throw std::runtime_error("Invalid padding");
        }
    }

    std::vector<uint8_t> result(len - pad_len);
    if (!result.empty()) {
        std::memcpy(result.data(), data, result.size());
    }
    return result;
}

std::string bytesToHexImpl(const uint8_t* data, const size_t len) {
    std::string result;
    result.reserve(len * 2);
    for (size_t i = 0; i < len; ++i) {
        result += std::format("{:02x}", data[i]);
    }
    return result;
}

void HexToBytes(std::string_view hex, uint8_t* out, const size_t expected_bytes) {
    ValidateHex(hex, expected_bytes);
    for (size_t i = 0; i < expected_bytes; ++i) {
        out[i] = static_cast<uint8_t>((HexNibble(hex[i * 2]) << 4) | HexNibble(hex[i * 2 + 1]));
    }
}

std::vector<uint8_t> HexToBytesVector(std::string_view hex) {
    if (hex.size() % 2 != 0) {
        throw std::runtime_error("Invalid hex length");
    }
    std::vector<uint8_t> result(hex.size() / 2);
    for (size_t i = 0; i < result.size(); ++i) {
        result[i] = static_cast<uint8_t>((HexNibble(hex[i * 2]) << 4) | HexNibble(hex[i * 2 + 1]));
    }
    return result;
}

void FillRandomBytes(uint8_t* out, const size_t len) {
    mbedtls_entropy_context entropy;
    mbedtls_ctr_drbg_context ctr_drbg;
    mbedtls_entropy_init(&entropy);
    mbedtls_ctr_drbg_init(&ctr_drbg);

    constexpr char kPersonalization[] = "Smile2Unlock.crypto";
    const int seed_result = mbedtls_ctr_drbg_seed(
        &ctr_drbg,
        mbedtls_entropy_func,
        &entropy,
        reinterpret_cast<const unsigned char*>(kPersonalization),
        sizeof(kPersonalization) - 1);
    if (seed_result != 0) {
        mbedtls_ctr_drbg_free(&ctr_drbg);
        mbedtls_entropy_free(&entropy);
        throw std::runtime_error(std::format("mbedtls_ctr_drbg_seed failed: {}", seed_result));
    }

    const int random_result = mbedtls_ctr_drbg_random(&ctr_drbg, out, len);
    mbedtls_ctr_drbg_free(&ctr_drbg);
    mbedtls_entropy_free(&entropy);
    if (random_result != 0) {
        throw std::runtime_error(std::format("mbedtls_ctr_drbg_random failed: {}", random_result));
    }
}

} // namespace

export
class Crypto {
private:
    std::array<uint8_t, kAesKeySize> key_{};
    std::array<uint8_t, kAesIvSize> iv_{};

    static std::array<uint8_t, kAesKeySize> generateRandomKey() {
        std::array<uint8_t, kAesKeySize> bytes{};
        FillRandomBytes(bytes.data(), bytes.size());
        return bytes;
    }

    static std::array<uint8_t, kAesIvSize> generateRandomIv() {
        std::array<uint8_t, kAesIvSize> bytes{};
        FillRandomBytes(bytes.data(), bytes.size());
        return bytes;
    }

public:
    Crypto() {
        generateKeyAndIV();
    }

    void generateKeyAndIV() {
        key_ = generateRandomKey();
        iv_ = generateRandomIv();
    }

    std::string getKeyHex() const {
        return bytesToHexImpl(key_.data(), key_.size());
    }

    std::string getIvHex() const {
        return bytesToHexImpl(iv_.data(), iv_.size());
    }

    bool setKeyHex(std::string_view keyHex) {
        try {
            HexToBytes(keyHex, key_.data(), key_.size());
            return true;
        } catch (const std::exception& e) {
            std::cerr << std::format("Key parse error: {}\n", e.what());
            return false;
        }
    }

    bool setIvHex(std::string_view ivHex) {
        try {
            HexToBytes(ivHex, iv_.data(), iv_.size());
            return true;
        } catch (const std::exception& e) {
            std::cerr << std::format("IV parse error: {}\n", e.what());
            return false;
        }
    }

    static std::string bytesToHex(std::span<const uint8_t> bytes) {
        return bytesToHexImpl(bytes.data(), bytes.size());
    }

    static std::vector<uint8_t> hexToBytes(std::string_view hex) {
        try {
            return HexToBytesVector(hex);
        } catch (const std::exception& e) {
            std::cerr << std::format("Hex parse error: {}\n", e.what());
            return {};
        }
    }

    std::string encrypt(std::string_view plaintext) const {
        try {
            auto padded = AddPadding(reinterpret_cast<const uint8_t*>(plaintext.data()), plaintext.size());
            std::vector<uint8_t> ciphertext(padded.size());
            std::array<uint8_t, kAesIvSize> iv_copy = iv_;

            mbedtls_aes_context aes;
            mbedtls_aes_init(&aes);
            const int setkey_result = mbedtls_aes_setkey_enc(&aes, key_.data(), static_cast<unsigned int>(key_.size() * 8));
            if (setkey_result != 0) {
                mbedtls_aes_free(&aes);
                throw std::runtime_error(std::format("mbedtls_aes_setkey_enc failed: {}", setkey_result));
            }

            const int crypt_result = mbedtls_aes_crypt_cbc(
                &aes,
                MBEDTLS_AES_ENCRYPT,
                padded.size(),
                iv_copy.data(),
                padded.data(),
                ciphertext.data());
            mbedtls_aes_free(&aes);
            if (crypt_result != 0) {
                throw std::runtime_error(std::format("mbedtls_aes_crypt_cbc failed: {}", crypt_result));
            }

            return std::string(reinterpret_cast<const char*>(ciphertext.data()), ciphertext.size());
        } catch (const std::exception& e) {
            std::cerr << std::format("Encryption error: {}\n", e.what());
            return {};
        }
    }

    std::string decrypt(std::string_view ciphertext) const {
        try {
            if (ciphertext.empty() || ciphertext.size() % kAesBlockSize != 0) {
                std::cerr << "Decryption error: invalid ciphertext length\n";
                return {};
            }

            std::vector<uint8_t> decrypted(ciphertext.size());
            std::array<uint8_t, kAesIvSize> iv_copy = iv_;

            mbedtls_aes_context aes;
            mbedtls_aes_init(&aes);
            const int setkey_result = mbedtls_aes_setkey_dec(&aes, key_.data(), static_cast<unsigned int>(key_.size() * 8));
            if (setkey_result != 0) {
                mbedtls_aes_free(&aes);
                throw std::runtime_error(std::format("mbedtls_aes_setkey_dec failed: {}", setkey_result));
            }

            const int crypt_result = mbedtls_aes_crypt_cbc(
                &aes,
                MBEDTLS_AES_DECRYPT,
                ciphertext.size(),
                iv_copy.data(),
                reinterpret_cast<const unsigned char*>(ciphertext.data()),
                decrypted.data());
            mbedtls_aes_free(&aes);
            if (crypt_result != 0) {
                throw std::runtime_error(std::format("mbedtls_aes_crypt_cbc failed: {}", crypt_result));
            }

            auto unpadded = RemovePadding(decrypted.data(), decrypted.size());
            return std::string(reinterpret_cast<const char*>(unpadded.data()), unpadded.size());
        } catch (const std::exception& e) {
            std::cerr << std::format("Decryption error: {}\n", e.what());
            return {};
        }
    }
};
