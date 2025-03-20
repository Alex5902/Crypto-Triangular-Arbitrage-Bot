#ifndef KEY_ENCRYPTOR_HPP
#define KEY_ENCRYPTOR_HPP

#include <string>

namespace KeyEncryptor {

/**
 * Encrypts the given plaintext with the passphrase using AES-256-CBC.
 * Returns a base64-encoded string of (iv + ciphertext).
 */
std::string encryptData(const std::string& passphrase,
                        const std::string& plaintext);

/**
 * Decrypts a base64-encoded (iv + ciphertext) using the passphrase.
 */
std::string decryptData(const std::string& passphrase,
                        const std::string& base64Cipher);

} // namespace KeyEncryptor

// Declare this outside the namespace for encrypt_keys.cpp
void encryptKeysToFile(const std::string& apiKey,
                       const std::string& secretKey,
                       const std::string& passphrase,
                       const std::string& outputFilePath);

#endif // KEY_ENCRYPTOR_HPP
