#include "exchange/binance_account_sync.hpp"
#include <chrono>
#include <iostream>
#include <sstream>
#include <thread>
#include <curl/curl.h>
#include <openssl/hmac.h>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

static std::string hmac_sha256(const std::string& key, const std::string& data) {
    unsigned char* digest;
    digest = HMAC(EVP_sha256(),
                  key.c_str(), key.length(),
                  reinterpret_cast<const unsigned char*>(data.c_str()), data.length(),
                  nullptr, nullptr);

    std::ostringstream result;
    for (int i = 0; i < 32; ++i)
        result << std::hex << std::setw(2) << std::setfill('0') << (int)digest[i];
    return result.str();
}

static size_t writeCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    size_t totalSize = size * nmemb;
    out->append((char*)contents, totalSize);
    return totalSize;
}

void startWalletSyncThread(Wallet* wallet,
                           const std::string& apiKey,
                           const std::string& secretKey,
                           const std::string& baseUrl,
                           std::atomic<bool>* keepRunning,
                           std::thread& syncThread)
{
    syncThread = std::thread([=]() {
        CURL* curl = curl_easy_init();
        if (!curl) {
            std::cerr << "[SYNC] Failed to init curl\n";
            return;
        }

        while (keepRunning->load()) {
            try {
                long nowMs = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::system_clock::now().time_since_epoch()
                ).count();

                std::ostringstream qs;
                qs << "recvWindow=5000&timestamp=" << nowMs;
                std::string query = qs.str();
                std::string sig = hmac_sha256(secretKey, query);
                query += "&signature=" + sig;

                std::string url = baseUrl + "/api/v3/account?" + query;

                struct curl_slist* headers = nullptr;
                headers = curl_slist_append(headers, ("X-MBX-APIKEY: " + apiKey).c_str());

                std::string response;
                curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
                curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
                curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeCallback);
                curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
                curl_easy_setopt(curl, CURLOPT_TIMEOUT, 10L);

                CURLcode res = curl_easy_perform(curl);
                if (res != CURLE_OK) {
                    std::cerr << "[SYNC] CURL error: " << curl_easy_strerror(res) << "\n";
                } else {
                    auto j = json::parse(response);
                    if (j.contains("balances") && j["balances"].is_array()) {
                        for (const auto& b : j["balances"]) {
                            std::string asset = b["asset"];
                            double free = std::stod(b["free"].get<std::string>());
                            double locked = std::stod(b["locked"].get<std::string>());
                            double total = free + locked;
                            if (total > 0.0) {
                                wallet->setBalance(asset, total);
                            }
                        }
                        std::cout << "[SYNC] Wallet balances updated.\n";
                    }
                }

                curl_slist_free_all(headers);
            } catch (...) {
                std::cerr << "[SYNC] Exception during wallet sync\n";
            }

            std::this_thread::sleep_for(std::chrono::seconds(5));
        }

        curl_easy_cleanup(curl);
    });
}
