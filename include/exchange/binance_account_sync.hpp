#ifndef BINANCE_ACCOUNT_SYNC_HPP
#define BINANCE_ACCOUNT_SYNC_HPP

#include <string>
#include <thread>
#include <atomic>
#include "core/wallet.hpp"

// Starts a background thread that periodically syncs wallet balances with Binance.
void startWalletSyncThread(Wallet* wallet,
                           const std::string& apiKey,
                           const std::string& secretKey,
                           const std::string& baseUrl,
                           std::atomic<bool>* keepRunning,
                           std::thread& syncThread);

#endif // BINANCE_ACCOUNT_SYNC_HPP
