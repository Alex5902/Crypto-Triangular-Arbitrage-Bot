#ifndef TRIANGLE_SCANNER_HPP
#define TRIANGLE_SCANNER_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include "core/thread_pool.hpp" // if you have your thread pool

// Define the full struct here
struct Triangle {
    std::string base;
    std::vector<std::string> path;
};

class OrderBookManager; // forward declare if needed

class TriangleScanner {
public:
    TriangleScanner();

    void setOrderBookManager(OrderBookManager* obm);
    void loadTrianglesFromFile(const std::string& filepath);
    void scanTrianglesForSymbol(const std::string& symbol); // partial re-scan
    double calculateProfit(const Triangle& tri);

    void setMinProfitThreshold(double thresh) { minProfitThreshold_ = thresh; }

private:
    OrderBookManager* obm_{nullptr};

    // now we have a complete type for Triangle
    std::vector<Triangle> triangles_;
    std::unordered_map<std::string, std::vector<int>> symbolToTriangles_;

    double minProfitThreshold_{0.0};

    ThreadPool pool_{4};  // If you do the concurrency approach
};

#endif
