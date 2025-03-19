#ifndef SIMULATOR_HPP
#define SIMULATOR_HPP

#include <string>
#include <fstream>
#include "core/orderbook.hpp" // for OrderBookData

struct Triangle;

/**
 * A simple wallet tracking balances of BTC, ETH, USDT.
 */
struct Wallet {
    double btc;
    double eth;
    double usdt;
};

/**
 * The simulator now:
 *  - tracks a wallet (BTC, ETH, USDT)
 *  - enforces volume limits, slippage tolerance, min fill ratio
 *  - partial-fill approach across multiple levels
 */
class Simulator {
public:
    Simulator(const std::string& logFileName,
              double feePercent,
              double slippageTolerance,
              double volumeLimit,
              double minFillRatio);

    // On startup, we could set some initial amounts:
    void setInitialBalances(double btc, double eth, double usdt);

    // new partial-fill approach with wallet awareness
    // We pick how much to trade based on wallet holdings + volumeLimit
    bool simulateTradeDepthWithWallet(const Triangle& tri,
                                      const OrderBookData& ob1,
                                      const OrderBookData& ob2,
                                      const OrderBookData& ob3);

    // Just for reference – old function if you want it
    double simulateTrade(const Triangle& tri, double currentBalance,
                         double bid1, double ask1,
                         double bid2, double ask2,
                         double bid3, double ask3);

    // Print or retrieve wallet
    void printWallet() const;
    Wallet getWallet() const { return wallet_; }

private:
    // Logging function
    void logTrade(const std::string& path,
                  double startVal,
                  double endVal,
                  double profitPercent);

    // The partial-fill helpers
    // 'side' indicates buy or sell from the perspective of the base asset
    // e.g. "BTCUSDT" => if we hold BTC, we SELL for USDT
    double fillSell(const std::vector<OrderBookLevel>& bids,
                    double volumeToSell,
                    double& actualPrice,
                    double& filledVolume);

    double fillBuy(const std::vector<OrderBookLevel>& asks,
                   double quoteAmount,
                   double& actualPrice,
                   double& filledVolume);

    // doOneStep that modifies the wallet in-place
    bool doOneStepWithWallet(const std::string& pairName,
                             double slippageTopPrice,
                             double volumeLimit);

private:
    std::string logFileName_;
    double feePercent_;
    double slippageTolerance_;   // e.g. 0.01 means we allow up to 1% worse than top-of-book
    double volumeLimit_;         // max volume in base we’re willing to trade
    double minFillRatio_;        // require at least e.g. 80% of requested fill or we abort

    Wallet wallet_; // track BTC, ETH, USDT
};

#endif
