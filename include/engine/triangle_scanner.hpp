#ifndef TRIANGLE_SCANNER_HPP
#define TRIANGLE_SCANNER_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include <future>
#include <mutex>

#include "core/thread_pool.hpp"
#include "core/triangle.hpp"  // struct Triangle

// forward-declare
class OrderBookManager;
class Simulator;

/**
 * Now includes:
 *  1) Additional concurrency method scanAllSymbolsConcurrently(...)
 *  2) A function to log scanning to CSV (scan_log.csv)
 */
class TriangleScanner {
public:
    TriangleScanner();

    void setOrderBookManager(OrderBookManager* obm);

    void loadTrianglesFromFile(const std::string& filepath);
    void scanTrianglesForSymbol(const std::string& symbol);

    double calculateProfit(const Triangle& tri);

    void setMinProfitThreshold(double thresh) { minProfitThreshold_ = thresh; }
    void setSimulator(Simulator* sim) { simulator_ = sim; }

    // Additional concurrency method
    void scanAllSymbolsConcurrently();

private:
    // CSV logger for scanning
    void logScanResult(const std::string& symbol,
                       int triCount,
                       double bestProfit,
                       double latencyMs);

private:
    OrderBookManager* obm_{nullptr};

    std::vector<Triangle> triangles_;
    std::unordered_map<std::string, std::vector<int>> symbolToTriangles_;

    double minProfitThreshold_{0.0};
    ThreadPool pool_{4};

    Simulator* simulator_{nullptr};

    // For concurrency around writing to scan_log.csv
    std::mutex scanLogMutex_;
    bool scanLogHeaderWritten_{false};
};

#endif // TRIANGLE_SCANNER_HPP
