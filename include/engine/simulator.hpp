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
 * parseSymbol => given "BTCUSDT" returns {"BTC","USDT"} 
 */
std::pair<std::string,std::string> parseSymbol(const std::string& pair);

/**
 * The Simulator can run in liveMode or dryMode:
 *  - If liveMode_ == false => pure depth-based simulation
 *  - If liveMode_ == true  => calls placeMarketOrder(...) on your real or dry executor
 * 
 * Now includes a "slippage-adjusted profitability" pre-check.
 */
class Simulator {
public:
    Simulator(const std::string& logFileName,
              double feePercent,
              double slippageTolerance,
              double volumeLimit,
              double minFillRatio,
              Wallet* sharedWallet,
              IExchangeExecutor* executor,
              double minProfitUSDT);

    void setLiveMode(bool live) { liveMode_ = live; }

    /**
     * The main 3-leg function. 
     * 1) Estimates final USDT if all legs fill with no big slippage.
     * 2) If profitable => does the real trade (live or sim).
     */
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
    // Helper to do a partial "dry-run" for the 3 legs to see final USDT
    // This does NOT modify the wallet. It just estimates final USDT after 3 legs.
    // Returns final USDT or <0 on failure.
    double estimateTriangleProfit(const Triangle& tri,
                                  const OrderBookData& ob1,
                                  const OrderBookData& ob2,
                                  const OrderBookData& ob3,
                                  double startValUSDT);

    // The normal "simulate or live-trade" code for each leg
    bool doLeg(WalletTransaction& tx,
               const std::string& pairName,
               const OrderBookData& ob);

    bool doLegLive(WalletTransaction& tx,
                   const std::string& pairName,
                   double desiredQtyBase,
                   bool isSell);

    // figure out which assets are used by "BTCUSDT" => lock them
    std::vector<std::string> getAssetsForPair(const std::string& pairName) const;

    // Logging
    void logTrade(const std::string& path,
                  double startVal,
                  double endVal,
                  double profitPercent);

    void logLeg(const std::string& pairName,
                const std::string& side,
                double requestedQty,
                double filledQty,
                double fillRatio,
                double slipPct,
                double latencyMs);

private:
    std::string logFileName_;
    double feePercent_;
    double slippageTolerance_;
    double volumeLimit_;
    double minFillRatio_;
    double minProfitUSDT_;

    Wallet* wallet_;
    IExchangeExecutor* executor_;

    bool liveMode_{false};

    // Shared locks across assets
    static std::map<std::string, std::mutex> assetLocks_;

    int totalTrades_{0};
    double cumulativeProfit_{0.0};
};

#endif // SIMULATOR_HPP
