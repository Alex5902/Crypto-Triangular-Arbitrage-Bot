#ifndef WALLET_HPP
#define WALLET_HPP

#include <string>
#include <unordered_map>
#include <mutex>
#include <vector>

/**
 * For multi-leg atomic trades, we define:
 */
struct WalletChange {
    std::string asset;
    double deltaBalance; // +/- adjustments to total
    double deltaLocked;  // +/- adjustments to locked portion
};

struct WalletTransaction {
    bool active;
    std::vector<WalletChange> changes;
};

class Wallet {
public:
    Wallet();

    void setBalance(const std::string& asset, double amount);

    // free = total - locked
    double getFreeBalance(const std::string& asset) const;

    // total
    double getTotalBalance(const std::string& asset) const;

    // Start a multi-leg transaction
    WalletTransaction beginTransaction();

    // applyChange => modifies wallet in place if valid
    bool applyChange(WalletTransaction& tx,
                     const std::string& asset,
                     double deltaBalance,
                     double deltaLocked);

    // commit => finalize
    bool commitTransaction(WalletTransaction& tx);

    // rollback => revert changes
    void rollbackTransaction(WalletTransaction& tx);

    // debug
    void printAll() const;

private:
    std::unordered_map<std::string,double> balances_;
    std::unordered_map<std::string,double> locked_;

    mutable std::mutex walletMutex_;
};

#endif // WALLET_HPP
