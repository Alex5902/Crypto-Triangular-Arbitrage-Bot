#include "core/wallet.hpp"
#include <iostream>

Wallet::Wallet() {
    balances_["BTC"]  = 0.0;
    balances_["ETH"]  = 0.0;
    balances_["USDT"] = 0.0;

    locked_["BTC"]    = 0.0;
    locked_["ETH"]    = 0.0;
    locked_["USDT"]   = 0.0;
}

void Wallet::setBalance(const std::string& asset, double amount) {
    std::lock_guard<std::mutex> lk(walletMutex_);
    balances_[asset] = amount;
    if (!locked_.count(asset)) {
        locked_[asset] = 0.0;
    }
}

double Wallet::getFreeBalance(const std::string& asset) const {
    std::lock_guard<std::mutex> lk(walletMutex_);
    if (!balances_.count(asset)) return 0.0;
    double t = balances_.at(asset);
    double l = locked_.at(asset);
    double f = t - l;
    return (f<0.0? 0.0 : f);
}

double Wallet::getTotalBalance(const std::string& asset) const {
    std::lock_guard<std::mutex> lk(walletMutex_);
    if (!balances_.count(asset)) return 0.0;
    return balances_.at(asset);
}

WalletTransaction Wallet::beginTransaction() {
    WalletTransaction tx;
    tx.active = true;
    return tx;
}

bool Wallet::applyChange(WalletTransaction& tx,
                         const std::string& asset,
                         double deltaBalance,
                         double deltaLocked)
{
    if (!tx.active) return false;
    std::lock_guard<std::mutex> lk(walletMutex_);

    if (!balances_.count(asset)) {
        balances_[asset] = 0.0;
        locked_[asset]   = 0.0;
    }
    double newBal = balances_[asset] + deltaBalance;
    double newLock= locked_[asset]   + deltaLocked;

    if (newBal < 0.0 || newLock < 0.0) {
        return false;
    }
    if (newLock > newBal) {
        return false; // can't lock more than total
    }

    // record
    WalletChange c;
    c.asset = asset;
    c.deltaBalance = deltaBalance;
    c.deltaLocked  = deltaLocked;
    tx.changes.push_back(c);

    // apply
    balances_[asset] = newBal;
    locked_[asset]   = newLock;

    return true;
}

bool Wallet::commitTransaction(WalletTransaction& tx) {
    if (!tx.active) return false;
    tx.active = false;
    // changes are already applied in applyChange
    return true;
}

void Wallet::rollbackTransaction(WalletTransaction& tx) {
    if (!tx.active) return;
    tx.active = false;

    std::lock_guard<std::mutex> lk(walletMutex_);
    // revert in reverse order
    for (auto it = tx.changes.rbegin(); it != tx.changes.rend(); ++it) {
        auto &ch = *it;
        balances_[ch.asset] -= ch.deltaBalance;
        locked_[ch.asset]   -= ch.deltaLocked;
        if (balances_[ch.asset] < 0.0) balances_[ch.asset] = 0.0;
        if (locked_[ch.asset] < 0.0)   locked_[ch.asset] = 0.0;
    }
}

void Wallet::printAll() const {
    std::lock_guard<std::mutex> lk(walletMutex_);
    std::cout << "[WALLET] Balances:\n";
    for (auto &kv : balances_) {
        double l = locked_.at(kv.first);
        double f = kv.second - l;
        std::cout << "  " << kv.first 
                  << ": total=" << kv.second 
                  << " locked=" << l
                  << " free=" << f << "\n";
    }
}
