#include "exchange/key_encryptor.hpp"
#include <iostream>

int main() {
    std::string apiKey, secretKey, passphrase;

    std::cout << "Enter your Binance Testnet API Key: ";
    std::getline(std::cin, apiKey);

    std::cout << "Enter your Binance Testnet Secret Key: ";
    std::getline(std::cin, secretKey);

    std::cout << "Enter a passphrase to encrypt with: ";
    std::getline(std::cin, passphrase);

    encryptKeysToFile(apiKey, secretKey, passphrase, "config/keys.enc");
    std::cout << "Encrypted keys saved to config/keys.enc\n";
    return 0;
}
