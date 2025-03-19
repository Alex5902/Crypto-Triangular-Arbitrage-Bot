#ifndef SIMULATOR_HPP
#define SIMULATOR_HPP

#include <string>
#include <fstream>
#include "core/triangle.hpp"
#include "core/orderbook.hpp"
#include "core/wallet.hpp"
#include "exchange/i_exchange_executor.hpp"

/**
 * The simulator uses:
 *  - a pointer to your atomic Wallet
 *  - a pointer to an IExchangeExecutor (dry or real)
 *
 * It executes multi-leg trades in a transaction. If a leg fails => rollback.
 */
class Simulator {
public:
    Simulator(const std::string& logFileName,
              double feePercent,
              double slippageTolerance,
              double volumeLimit,
              double minFillRatio,
              Wallet* sharedWallet,
              IExchangeExecutor* executor);

    bool simulateTradeDepthWithWallet(const Triangle& tri,
                                      const OrderBookData& ob1,
                                      const OrderBookData& ob2,
                                      const OrderBookData& ob3);

    // older approach if you want it
    double simulateTrade(const Triangle& tri,
                         double currentBalance,
                         double bid1, double ask1,
                         double bid2, double ask2,
                         double bid3, double ask3);

    void printWallet() const;

private:
    void logTrade(const std::string& path,
                  double startVal,
                  double endVal,
                  double profitPercent);

    bool doLeg(WalletTransaction& tx, 
               const std::string& pairName,
               double topOfBookPrice);

private:
    std::string logFileName_;
    double feePercent_;
    double slippageTolerance_;
    double volumeLimit_;
    double minFillRatio_;

    Wallet* wallet_;
    IExchangeExecutor* executor_; // can be a DryExecutor or real
};

#endif // SIMULATOR_HPP
