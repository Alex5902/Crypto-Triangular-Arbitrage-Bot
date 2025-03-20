#include "exchange/key_encryptor.hpp"
#include <openssl/evp.h>
#include <openssl/rand.h>
#include <openssl/sha.h>
#include <stdexcept>
#include <vector>
#include <sstream>
#include <iomanip>
#include <cstring>    // for memcpy
#include <iostream>
#include <algorithm>
#include <cctype>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/bio.h>
#include <openssl/buffer.h>
#include <openssl/bio.h>

namespace {

std::string base64Encode(const unsigned char* buffer, size_t length) {
    BIO* bmem = BIO_new(BIO_s_mem());
    BIO* b64 = BIO_new(BIO_f_base64());
    // disable newlines
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    b64 = BIO_push(b64, bmem);
    BIO_write(b64, buffer, (int)length);
    (void)BIO_flush(b64);
    BUF_MEM* bptr = nullptr;
    BIO_get_mem_ptr(b64, &bptr);

    std::string encoded(bptr->data, bptr->length);
    BIO_free_all(b64);
    return encoded;
}

std::vector<unsigned char> base64Decode(const std::string& encoded) {
    BIO* b64 = BIO_new(BIO_f_base64());
    BIO_set_flags(b64, BIO_FLAGS_BASE64_NO_NL);
    BIO* bmem = BIO_new_mem_buf(encoded.c_str(), (int)encoded.size());
    bmem = BIO_push(b64, bmem);

    std::vector<unsigned char> buffer(encoded.size());
    int decodedSize = BIO_read(bmem, buffer.data(), (int)buffer.size());
    buffer.resize(decodedSize > 0 ? decodedSize : 0);

    BIO_free_all(bmem);
    return buffer;
}

/**
 * Derive a key from passphrase using a simple approach (SHA-256).
 * For production, you'd want PBKDF2 or Argon2. This is a simplified example.
 */
std::vector<unsigned char> deriveKey32(const std::string& passphrase) {
    // We do a single SHA-256 of passphrase for 32 bytes.
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256((const unsigned char*)passphrase.data(), passphrase.size(), hash);

    return std::vector<unsigned char>(hash, hash + 32);
}

} // anonymous namespace

namespace KeyEncryptor {

std::string encryptData(const std::string& passphrase,
                        const std::string& plaintext)
{
    // Derive key
    auto key = deriveKey32(passphrase);

    // Generate random IV (16 bytes for AES-256-CBC)
    unsigned char iv[16];
    if (!RAND_bytes(iv, 16)) {
        throw std::runtime_error("Failed to generate IV");
    }

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if(!ctx) {
        throw std::runtime_error("EVP_CIPHER_CTX_new failed");
    }

    if(1 != EVP_EncryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv)) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptInit_ex failed");
    }

    std::vector<unsigned char> ciphertext(plaintext.size() + 16);
    int outLen1=0;
    if(1 != EVP_EncryptUpdate(ctx, ciphertext.data(), &outLen1,
                              (const unsigned char*)plaintext.data(),
                              (int)plaintext.size())) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptUpdate failed");
    }

    int outLen2=0;
    if(1 != EVP_EncryptFinal_ex(ctx, ciphertext.data() + outLen1, &outLen2)) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_EncryptFinal_ex failed");
    }

    EVP_CIPHER_CTX_free(ctx);
    ciphertext.resize(outLen1 + outLen2);

    // Build final = (iv + ciphertext)
    std::vector<unsigned char> finalData;
    finalData.insert(finalData.end(), iv, iv+16);
    finalData.insert(finalData.end(), ciphertext.begin(), ciphertext.end());

    // base64-encode
    return base64Encode(finalData.data(), finalData.size());
}

std::string decryptData(const std::string& passphrase,
                        const std::string& base64Cipher)
{
    auto allBytes = base64Decode(base64Cipher);
    if (allBytes.size() < 16) {
        throw std::runtime_error("Cipher data too short, missing IV");
    }
    // first 16 = IV
    unsigned char iv[16];
    memcpy(iv, allBytes.data(), 16);

    // the rest = actual ciphertext
    std::vector<unsigned char> ciphertext(allBytes.begin()+16, allBytes.end());

    auto key = deriveKey32(passphrase);

    EVP_CIPHER_CTX* ctx = EVP_CIPHER_CTX_new();
    if(!ctx) {
        throw std::runtime_error("EVP_CIPHER_CTX_new failed (decrypt)");
    }
    if(1 != EVP_DecryptInit_ex(ctx, EVP_aes_256_cbc(), nullptr, key.data(), iv)) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptInit_ex failed");
    }

    std::vector<unsigned char> outBuf(ciphertext.size() + 16);
    int outLen1=0;
    if(1 != EVP_DecryptUpdate(ctx, outBuf.data(), &outLen1,
                              ciphertext.data(), (int)ciphertext.size())) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptUpdate failed");
    }

    int outLen2=0;
    if(1 != EVP_DecryptFinal_ex(ctx, outBuf.data() + outLen1, &outLen2)) {
        EVP_CIPHER_CTX_free(ctx);
        throw std::runtime_error("EVP_DecryptFinal_ex failed - possibly wrong passphrase");
    }

    EVP_CIPHER_CTX_free(ctx);
    outBuf.resize(outLen1 + outLen2);

    return std::string((char*)outBuf.data(), outBuf.size());
}

} // namespace KeyEncryptor

#include <fstream>
#include <nlohmann/json.hpp>

void encryptKeysToFile(const std::string& apiKey,
                       const std::string& secretKey,
                       const std::string& passphrase,
                       const std::string& outputFilePath)
{
    // Convert keys to JSON
    nlohmann::json j;
    j["apiKey"] = apiKey;
    j["secretKey"] = secretKey;

    std::string plain = j.dump();  // serialise to string
    std::string encrypted = KeyEncryptor::encryptData(passphrase, plain);

    std::ofstream out(outputFilePath);
    if (!out.is_open()) {
        throw std::runtime_error("Failed to write to " + outputFilePath);
    }
    out << encrypted;
    out.close();
}

