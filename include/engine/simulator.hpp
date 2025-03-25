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

// We'll declare parseSymbol here so we can use it in the .cpp
std::pair<std::string,std::string> parseSymbol(const std::string& pair);

class Simulator {
public:
    Simulator(const std::string& logFileName,
              double feePercent,
              double slippageTolerance,
              double volumeLimit,
              double minFillRatio,
              Wallet* sharedWallet,
              IExchangeExecutor* executor);

    /**
     * If liveMode is false => do a depth-based simulation.
     * If liveMode is true  => do actual trades with placeMarketOrder(...).
     */
    void setLiveMode(bool live) { liveMode_ = live; }

    bool simulateTradeDepthWithWallet(const Triangle& tri,
                                      const OrderBookData& ob1,
                                      const OrderBookData& ob2,
                                      const OrderBookData& ob3);

    double simulateTrade(const Triangle& tri,
                         double currentBalance,
                         double bid1, double ask1,
                         double bid2, double ask2,
                         double bid3, double ask3);

    void printWallet() const;

    // For TUI
    int getTotalTrades() const;
    double getCumulativeProfit() const;

private:
    // Logging for entire 3-leg trade
    void logTrade(const std::string& path,
                  double startVal,
                  double endVal,
                  double profitPercent);

    // If liveMode==false => the old doLeg simulation
    // If liveMode==true  => actual placeMarketOrder calls
    bool doLeg(WalletTransaction& tx,
               const std::string& pairName,
               const OrderBookData& ob);

    // For doLeg in live mode
    bool doLegLive(WalletTransaction& tx,
                   const std::string& pairName,
                   double desiredQtyBase,  // how many base units we want to fill
                   bool isSell);

    // log details about each leg
    void logLeg(const std::string& pairName,
                const std::string& side,
                double requestedQty,
                double filledQty,
                double fillRatio,
                double slipPct,
                double latencyMs);

    // Used only for locking assets needed by each triangle
    std::vector<std::string> getAssetsForPair(const std::string& pairName) const;

private:
    std::string logFileName_;
    double feePercent_;
    double slippageTolerance_;
    double volumeLimit_;
    double minFillRatio_;

    Wallet* wallet_;
    IExchangeExecutor* executor_;

    // NEW: determines if we do real trades or simulation
    bool liveMode_{false};

    static std::map<std::string, std::mutex> assetLocks_;

    int totalTrades_{0};
    double cumulativeProfit_{0.0};
};

#endif // SIMULATOR_HPP
