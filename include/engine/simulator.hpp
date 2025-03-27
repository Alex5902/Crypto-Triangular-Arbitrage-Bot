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

/**
 * Represents the minNotional / minQty filters for a given symbol.
 */
struct SymbolFilter {
    double minNotional; // e.g. 10.0
    double minQty;      // e.g. 0.0001
};

/**
 * parseSymbol => extracts base vs quote from a symbol string
 */
std::pair<std::string,std::string> parseSymbol(const std::string& pair);

/**
 * A small struct to hold simulation results for multiple triangles
 */
struct SimCandidate {
    int triIndex;            // index in your vector of triangles
    double estimatedProfit;  // e.g. final USDT gain
};

struct ReversibleLeg {
    bool success { false };         // Track if the leg succeeded
    std::string symbol;             // e.g. "BTCUSDT"
    bool sideSell;                  // true = SELL, false = BUY
    double filledQtyBase { 0.0 };   // how much base was filled
};

/**
 * Depth-aware simulator with optional live trades.
 * 
 * Includes concurrency methods for multi-triangle simulation.
 * Now includes a more robust "atomic" execution approach:
 * - If any leg fails, we attempt to revert or skip the entire trade.
 * - For real trades, we do a best-effort approach, but cannot truly revert
 *   a partially filled exchange order.
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
              double minProfitUSDT);

    void setLiveMode(bool live) { liveMode_ = live; }

    /**
     * The main "atomic" trading function. If it detects negative or insufficient profit,
     * it skips. If any leg fails, it rolls back the entire local wallet transaction.
     *
     * Real exchange trades can't be undone, so in a real scenario you'd do a "reversal" trade
     * if Leg 2 or 3 fails. See code comments below.
     * 
     * NEW: If you want the reason for a failure, pass a pointer to failReason.
     */
    bool simulateTradeDepthWithWallet(const Triangle& tri,
                                      const OrderBookData& ob1,
                                      const OrderBookData& ob2,
                                      const OrderBookData& ob3,
                                      std::string* failReason = nullptr); // UPDATED

    /**
     * Old signature for backward compatibility. Internally calls the new one without failReason.
     */
    bool simulateTradeDepthWithWallet(const Triangle& tri,
                                      const OrderBookData& ob1,
                                      const OrderBookData& ob2,
                                      const OrderBookData& ob3);

    /**
     * Offline profitability check. Loops through partial fills on each OB,
     * returns final net profit in USDT or -1 if fail.
     */
    double estimateTriangleProfitUSDT(const Triangle& tri,
                                      const OrderBookData& ob1,
                                      const OrderBookData& ob2,
                                      const OrderBookData& ob3);

    /**
     * Legacy leftover, not used now
     */
    double simulateTrade(const Triangle& tri,
                         double currentBalance,
                         double bid1, double ask1,
                         double bid2, double ask2,
                         double bid3, double ask3);

    void printWallet() const;

    int getTotalTrades() const;
    double getCumulativeProfit() const;

    // concurrency to estimate many triangles
    std::vector<SimCandidate> simulateMultipleTrianglesConcurrently(
        const std::vector<Triangle>& triangles);

    // optionally run real trades in sequence for the best N
    void executeTopCandidatesSequentially(const std::vector<Triangle>& triangles,
                                          const std::vector<SimCandidate>& simCandidates,
                                          int bestN,
                                          double minUSDTprofit);

    // optional CSV export
    void exportSimCandidatesCSV(const std::string& filename,
                                const std::vector<Triangle>& triangles,
                                const std::vector<SimCandidate>& candidates,
                                int topN=50);

private:
    // internal leg logic, either local or real
    bool doLeg(WalletTransaction& tx,
        const std::string& pairName,
        const OrderBookData& ob,
        ReversibleLeg* reversalOut = nullptr);

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
    void loadSymbolFilters(const std::string& path);
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

    double minProfitUSDT_;

    static std::map<std::string, std::mutex> assetLocks_;

    int totalTrades_{0};
    double cumulativeProfit_{0.0};

    void reverseRealLeg(const ReversibleLeg& leg);

    // symbol -> filter
    std::unordered_map<std::string, SymbolFilter> symbolFilters_;
};

#endif // SIMULATOR_HPP
