#include <iostream>
#include <string>
#include <string_view>
#include <span>
#include <format>
#include <array>
#include <algorithm>
#include <ranges>

#include <cryptopp/aes.h>
#include <cryptopp/modes.h>
#include <cryptopp/filters.h>
#include <cryptopp/hex.h>
#include <cryptopp/osrng.h>

namespace CryptoPP {
    using namespace CryptoPP;
}

/**
 * @class Crypto
 * @brief AES加密解密类，基于Crypto++库实现AES-CBC模式的加解密功能
 * 
 * 该类提供了密钥和IV的自动生成、获取和自定义设置功能，
 * 支持字符串的加密解密操作，并提供了十六进制格式的转换工具。
 */
class Crypto {
private:

    
public:

    /**
     * @brief AES密钥数组
     * 
     * 使用AES默认密钥长度（256位，32字节）
     */
    std::array<CryptoPP::byte, CryptoPP::AES::DEFAULT_KEYLENGTH> key;

    /**
     * @brief 初始化向量（IV）数组
     * 
     * 使用AES块大小（128位，16字节）
     */
    std::array<CryptoPP::byte, CryptoPP::AES::BLOCKSIZE> iv;

    /**
     * @brief 构造函数
     * 
     * 自动生成随机密钥和初始化向量
     */
    Crypto() {
        generateKeyAndIV();
    }
    
    /**
     * @brief 生成随机密钥和初始化向量
     * 
     * 使用Crypto++的AutoSeededRandomPool生成高质量的随机密钥和IV
     */
    void generateKeyAndIV() {
        CryptoPP::AutoSeededRandomPool rnd;
        rnd.GenerateBlock(key.data(), key.size());
        rnd.GenerateBlock(iv.data(), iv.size());
    }
    
    /**
     * @brief 获取十六进制格式的密钥
     * 
     * @return 十六进制字符串表示的密钥
     */
    std::string getKeyHex() const {
        return bytesToHex(std::span<const CryptoPP::byte>(key.data(), key.size()));
    }
    
    /**
     * @brief 获取十六进制格式的初始化向量
     * 
     * @return 十六进制字符串表示的IV
     */
    std::string getIvHex() const {
        return bytesToHex(std::span<const CryptoPP::byte>(iv.data(), iv.size()));
    }
    
    /**
     * @brief 设置自定义密钥（从十六进制字符串）
     * 
     * @param keyHex 十六进制字符串表示的密钥，长度必须为64个字符（32字节）
     * @return 成功返回true，失败返回false
     */
    bool setKeyHex(std::string_view keyHex) {
        try {
            // 检查密钥长度是否正确
            if (keyHex.size() != CryptoPP::AES::DEFAULT_KEYLENGTH * 2) {
                std::cerr << std::format("Key length error: expected {} hex characters, got {}\n", 
                                        CryptoPP::AES::DEFAULT_KEYLENGTH * 2, keyHex.size());
                return false;
            }
            
            // 转换十六进制字符串为字节数组
            std::vector<CryptoPP::byte> keyBytes = hexToBytes(keyHex);
            
            // 复制到密钥数组
            std::copy(keyBytes.begin(), keyBytes.end(), key.begin());
            return true;
        } catch (const CryptoPP::Exception& e) {
            std::cerr << std::format("Set key error: {}\n", e.what());
            return false;
        } catch (const std::exception& e) {
            std::cerr << std::format("Set key error: {}\n", e.what());
            return false;
        }
    }
    
    /**
     * @brief 设置自定义初始化向量（从十六进制字符串）
     * 
     * @param ivHex 十六进制字符串表示的IV，长度必须为32个字符（16字节）
     * @return 成功返回true，失败返回false
     */
    bool setIvHex(std::string_view ivHex) {
        try {
            // 检查IV长度是否正确
            if (ivHex.size() != CryptoPP::AES::BLOCKSIZE * 2) {
                std::cerr << std::format("IV length error: expected {} hex characters, got {}\n", 
                                        CryptoPP::AES::BLOCKSIZE * 2, ivHex.size());
                return false;
            }
            
            // 转换十六进制字符串为字节数组
            std::vector<CryptoPP::byte> ivBytes = hexToBytes(ivHex);
            
            // 复制到IV数组
            std::copy(ivBytes.begin(), ivBytes.end(), iv.begin());
            return true;
        } catch (const CryptoPP::Exception& e) {
            std::cerr << std::format("Set IV error: {}\n", e.what());
            return false;
        } catch (const std::exception& e) {
            std::cerr << std::format("Set IV error: {}\n", e.what());
            return false;
        }
    }
    
    /**
     * @brief 将字节数组转换为十六进制字符串
     * 
     * @param bytes 要转换的字节数组
     * @return 十六进制字符串表示的字节数组内容
     */
    static std::string bytesToHex(std::span<const CryptoPP::byte> bytes) {
        std::string result;
        CryptoPP::StringSource(
            bytes.data(), bytes.size(), true,
            new CryptoPP::HexEncoder(new CryptoPP::StringSink(result))
        );
        return result;
    }
    
    /**
     * @brief 将十六进制字符串转换回字节数组
     * 
     * @param hex 十六进制字符串
     * @return 转换后的字节数组
     */
    static std::vector<CryptoPP::byte> hexToBytes(std::string_view hex) {
        std::vector<CryptoPP::byte> result(hex.size() / 2);
        CryptoPP::StringSource(
            reinterpret_cast<const CryptoPP::byte*>(std::string(hex).data()), hex.size(), true,
            new CryptoPP::HexDecoder(new CryptoPP::ArraySink(result.data(), result.size()))
        );
        return result;
    }
    
    /**
     * @brief 使用AES-CBC模式加密数据
     * 
     * @param plaintext 要加密的明文字符串
     * @return 加密后的密文字符串（二进制数据），失败返回空字符串
     */
    std::string encrypt(std::string_view plaintext) const {
        std::string ciphertext;
        
        try {
            // 创建AES-CBC加密器
            CryptoPP::CBC_Mode<CryptoPP::AES>::Encryption encryptor;
            encryptor.SetKeyWithIV(key.data(), key.size(), iv.data());
            
            // 执行加密操作
            CryptoPP::StringSource(
                reinterpret_cast<const CryptoPP::byte*>(plaintext.data()), plaintext.size(), true,
                new CryptoPP::StreamTransformationFilter(
                    encryptor,
                    new CryptoPP::StringSink(ciphertext)
                )
            );
        } catch (const CryptoPP::Exception& e) {
            std::cerr << std::format("Encryption error: {}\n", e.what());
            return {};
        }
        
        return ciphertext;
    }
    
    /**
     * @brief 使用AES-CBC模式解密数据
     * 
     * @param ciphertext 要解密的密文字符串（二进制数据）
     * @return 解密后的明文字符串，失败返回空字符串
     */
    std::string decrypt(std::string_view ciphertext) const {
        std::string decryptedtext;
        
        try {
            // 创建AES-CBC解密器
            CryptoPP::CBC_Mode<CryptoPP::AES>::Decryption decryptor;
            decryptor.SetKeyWithIV(key.data(), key.size(), iv.data());
            
            // 执行解密操作
            CryptoPP::StringSource(
                reinterpret_cast<const CryptoPP::byte*>(ciphertext.data()), ciphertext.size(), true,
                new CryptoPP::StreamTransformationFilter(
                    decryptor,
                    new CryptoPP::StringSink(decryptedtext)
                )
            );
        } catch (const CryptoPP::Exception& e) {
            std::cerr << std::format("Decryption error: {}\n", e.what());
            return {};
        }
        
        return decryptedtext;
    }
};