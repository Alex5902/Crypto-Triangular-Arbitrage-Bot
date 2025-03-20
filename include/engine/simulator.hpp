#ifndef SIMULATOR_HPP
#define SIMULATOR_HPP

#include <string>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>

#include "core/triangle.hpp"
#include "core/orderbook.hpp"
#include "core/wallet.hpp"
#include "exchange/i_exchange_executor.hpp"

/**
 * Depth-aware simulator:
 *  - For each leg, we read the entire order book (bids or asks)
 *  - We fill as much as we can across multiple price levels
 *  - Weighted-average fill price
 *  - Then check fill ratio & slippage
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

    // For TUI:
    int getTotalTrades() const;
    double getCumulativeProfit() const;

private:
    // Logging for entire 3-leg trade
    void logTrade(const std::string& path,
                  double startVal,
                  double endVal,
                  double profitPercent);

    // Depth-aware approach for each leg
    bool doLeg(WalletTransaction& tx,
               const std::string& pairName,
               const OrderBookData& ob);

    // Additional: log details about each leg
    void logLeg(const std::string& pairName,
                const std::string& side,
                double requestedQty,
                double filledQty,
                double fillRatio,
                double slipPct,
                double latencyMs);

    // figure out which assets are used by "BTCUSDT" => lock "BTC"+"USDT", etc.
    std::vector<std::string> getAssetsForPair(const std::string& pairName) const;

private:
    std::string logFileName_;
    double feePercent_;
    double slippageTolerance_;
    double volumeLimit_;
    double minFillRatio_;

    Wallet* wallet_;
    IExchangeExecutor* executor_; // can be unused in depth simulation, but kept for structure

    // Global locks: asset -> mutex
    static std::map<std::string, std::mutex> assetLocks_;

    // For TUI:
    int totalTrades_{0};
    double cumulativeProfit_{0.0};
};

#endif // SIMULATOR_HPP
