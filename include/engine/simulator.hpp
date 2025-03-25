#ifndef SIMULATOR_HPP
#define SIMULATOR_HPP

#include <string>
#include <fstream>
#include <map>
#include <mutex>
#include <vector>
#include <unordered_map>

#include "core/triangle.hpp"
#include "core/orderbook.hpp"
#include "core/wallet.hpp"
#include "exchange/i_exchange_executor.hpp"

// We'll declare parseSymbol here so we can use it in the .cpp
std::pair<std::string,std::string> parseSymbol(const std::string& pair);

/**
 * Represents the minNotional / minQty filters for a given symbol.
 */
struct SymbolFilter {
    double minNotional; // e.g. 10.0
    double minQty;      // e.g. 0.0001
};

/**
 * Depth-aware simulator with optional live trades.
 *
 * Includes:
 *   - estimateTriangleProfitUSDT(...) for full 3-leg pre-check
 *   - enforcement of minNotional / minQty filters
 */
class Simulator {
public:
    Simulator(const std::string& logFileName,
              double feePercent,
              double slippageTolerance,
              double maxFractionPerTrade,
              double minFillRatio,
              Wallet* sharedWallet,
              IExchangeExecutor* executor,
              double minProfitUSDT);  // <-- constructor

    // toggles real trades or simulation
    void setLiveMode(bool live) { liveMode_ = live; }

    bool simulateTradeDepthWithWallet(const Triangle& tri,
                                      const OrderBookData& ob1,
                                      const OrderBookData& ob2,
                                      const OrderBookData& ob3);

    // estimate final USDT if we fully execute tri with your depth-based sim
    double estimateTriangleProfitUSDT(const Triangle& tri,
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
    bool doLeg(WalletTransaction& tx,
               const std::string& pairName,
               const OrderBookData& ob);

    bool doLegLive(WalletTransaction& tx,
                   const std::string& pairName,
                   double desiredQtyBase,
                   bool isSell);

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

    std::vector<std::string> getAssetsForPair(const std::string& pairName) const;

    // NEW: load filters from JSON
    void loadSymbolFilters(const std::string& path);

    // helper to check filters on a trade (local sim or live)
    bool passesExchangeFilters(const std::string& symbol,
                               double quantityBase,
                               double priceEstimate);

private:
    std::string logFileName_;
    double feePercent_;
    double slippageTolerance_;
    double maxFractionPerTrade_;
    double minFillRatio_;

    Wallet* wallet_;
    IExchangeExecutor* executor_;

    bool liveMode_{false};

    double minProfitUSDT_; //<-- user-defined minProfit threshold

    // Global locks: asset -> mutex
    static std::map<std::string, std::mutex> assetLocks_;

    int totalTrades_{0};
    double cumulativeProfit_{0.0};

    // NEW: symbol -> filter (minNotional, minQty)
    std::unordered_map<std::string, SymbolFilter> symbolFilters_;
};

#endif // SIMULATOR_HPP
