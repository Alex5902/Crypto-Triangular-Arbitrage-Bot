#ifndef TRIANGLE_SCANNER_HPP
#define TRIANGLE_SCANNER_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include <future>
#include <mutex>
#include <map>
#include <queue>
#include <chrono>
#include "core/thread_pool.hpp"
#include "core/triangle.hpp"

class OrderBookManager;
class Simulator;

/**
 * We'll store a simple structure for our priority queue
 */
struct TriPriority {
    double profit;
    int triIdx; // index into triangles_ vector
    // Priority queue: we want largest 'profit' on top
    bool operator<(const TriPriority& other) const {
        return profit < other.profit;
    }
};

/**
 * A simple structure for exporting top triangles
 */
struct ScoredTriangle {
    int triIdx;
    double profit;     // e.g. +2.5 (%)
    double netUSDT;    // optional if you want net USDT
};

/**
 * TriangleScanner
 * - Loads triangles either from file or /exchangeInfo
 * - BFS-based approach to build them
 * - Scans them when a symbol's orderbook updates
 * - Maintains a priority queue of best-profit triangles
 * - Tracks last-known profit for each triangle
 *
 * Now includes:
 * - A cooldown to avoid spamming the same triangle repeatedly.
 */
class TriangleScanner {
public:
    TriangleScanner();

    void setOrderBookManager(OrderBookManager* obm);

    // Old (optional) file-based loading
    void loadTrianglesFromFile(const std::string& filepath);

    // Dynamically fetch from Binance exchangeInfo => BFS-based approach
    bool loadTrianglesFromBinanceExchangeInfo();

    // Called by OrderBookManager or user to re-check a symbol
    void scanTrianglesForSymbol(const std::string& symbol);

    // Full concurrency scanning
    void scanAllSymbolsConcurrently();

    // Single-triangle naive profit check
    double calculateProfit(const Triangle& tri);

    void setMinProfitThreshold(double thresh) { minProfitThreshold_ = thresh; }
    void setSimulator(Simulator* sim) { simulator_ = sim; }

    // For partial usage from existing code: get the current best triangle from the priority queue
    bool getBestTriangle(double& outProfit, Triangle& outTri);

    /**
     * Re-check all discovered triangles in parallel, store results in bestTriangles_, 
     * optionally also return a sorted vector for the user.
     * 
     * @param minProfitPct: skip updating bestTriangles_ for triangles below this profit
     * @param outSorted: if non-null, we fill it with all triangles above minProfit, sorted desc
     */
    void rescoreAllTrianglesConcurrently(
        double minProfitPct = 0.0,
        std::vector<ScoredTriangle>* outSorted = nullptr);

    /**
     * Export top triangles to CSV, e.g. "profitable_cycles.csv".
     * 
     * @param filename: path to CSV
     * @param topN: number of triangles to export
     * @param minProfitPct: only export triangles above this profit
     */
    void exportTopTrianglesCSV(const std::string& filename,
                               int topN,
                               double minProfitPct=0.0);

    // NEW: set the cooldown in seconds for each triangle
    void setTriangleCooldownSeconds(double secs) { triangleCooldownSeconds_ = secs; }

private:
    // BFS-based approach
    void buildTrianglesBFS(const std::unordered_map<std::string,
                         std::vector<std::pair<std::string,std::string>>>& adjacency);

    void logScanResult(const std::string& symbol,
                       int triCount,
                       double bestProfit,
                       double latencyMs);

    void updateTrianglePriority(int triIdx, double profit);

    std::string makeTriangleKey(const Triangle& tri) const;

    // -----------------------------------------------------------------------
    // NEW: Data + methods for blacklisting repeated failures
    // -----------------------------------------------------------------------
private:
    // track # of recent fails for each triangle
    std::unordered_map<std::string, std::vector<std::chrono::steady_clock::time_point>> failTimestamps_;  // NEW
    int maxFailsInWindow_{3};    // e.g. 3 fails in the last 60s => blacklisted  // NEW
    double failWindowSec_{60.0}; // e.g. 60s time window                        // NEW
    std::mutex failMutex_;       // for concurrent writes to failTimestamps_    // NEW

    // Record a failure for tri => push back current time, prune old entries   // NEW
    void recordFailure(const Triangle& tri, const std::string& reason);        // NEW

    // Check if a triangle is currently blacklisted (exceeded fail threshold)  // NEW
    bool isBlacklisted(const Triangle& tri);                                   // NEW

    // Log each failure reason to a CSV for debugging
    void logFailure(const Triangle& tri, const std::string& reason);           // NEW


private:
    OrderBookManager* obm_{nullptr};
    std::vector<Triangle> triangles_;

    // Reverse index: symbol => which triangles reference that symbol
    std::unordered_map<std::string, std::vector<int>> symbolToTriangles_;

    double minProfitThreshold_{0.0};
    ThreadPool pool_{4};
    Simulator* simulator_{nullptr};

    // CSV logging
    std::mutex scanLogMutex_;
    bool scanLogHeaderWritten_{false};

    // Track last-known profit for each triangle
    std::vector<double> lastProfits_;

    // Priority queue of TriPriority items
    std::priority_queue<TriPriority> bestTriangles_;
    std::mutex bestTriMutex_;

    // COOL DOWN
    double triangleCooldownSeconds_{10.0}; // e.g. 10s default
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastAttemptMap_;
    std::mutex cooldownMutex_;
};

#endif // TRIANGLE_SCANNER_HPP
