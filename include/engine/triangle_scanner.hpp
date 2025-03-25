#ifndef TRIANGLE_SCANNER_HPP
#define TRIANGLE_SCANNER_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include <future>
#include <mutex>
#include <map>
#include <queue>
#include "core/thread_pool.hpp"
#include "core/triangle.hpp"

// forward-declare
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
 * - Optionally uses BFS-based 3-asset cycle detection
 * - Scans them when a symbol's orderbook updates
 * - Maintains a priority queue of best-profit triangles
 * - Tracks last-known profit for each triangle
 *
 * NEW:
 * - rescoreAllTrianglesConcurrently(): re-check all triangles in parallel,
 *   update bestTriangles_, optionally store a sorted list of top triangles
 * - exportTopTrianglesCSV(): write the top triangles to a CSV
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
     * NEW: Re-check all discovered triangles in parallel, store results in bestTriangles_, 
     * optionally also return a sorted vector for the user.
     * @param minProfitPct: skip updating bestTriangles_ for triangles below this profit
     * @param outSorted: if non-null, we fill it with all triangles above minProfit, sorted desc
     */
    void rescoreAllTrianglesConcurrently(
        double minProfitPct = 0.0,
        std::vector<ScoredTriangle>* outSorted = nullptr);

    /**
     * NEW: Export top triangles to CSV, e.g. "profitable_cycles.csv".
     * You can call after rescoreAllTrianglesConcurrently().
     * 
     * @param filename: path to CSV
     * @param topN: number of triangles to export
     * @param minProfitPct: only export triangles above this profit
     */
    void exportTopTrianglesCSV(const std::string& filename,
                               int topN,
                               double minProfitPct=0.0);

private:
    // Logging
    void logScanResult(const std::string& symbol,
                       int triCount,
                       double bestProfit,
                       double latencyMs);

    // Old triple-loop approach (O(N^3)) if you still want it
    void buildTrianglesDynamically(const std::vector<std::string>& allSymbols,
                                   const std::map<std::string,std::string>& baseMap,
                                   const std::map<std::string,std::string>& quoteMap);

    // BFS-based approach
    void buildTrianglesBFS(const std::unordered_map<std::string,
                         std::vector<std::pair<std::string,std::string>>>& adjacency);

    // Called after scanning to push updated profits into the priority queue
    void updateTrianglePriority(int triIdx, double profit);

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
};

#endif // TRIANGLE_SCANNER_HPP
