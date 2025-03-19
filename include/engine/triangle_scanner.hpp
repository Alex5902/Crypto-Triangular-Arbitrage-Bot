#ifndef TRIANGLE_SCANNER_HPP
#define TRIANGLE_SCANNER_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include <future>
#include "core/thread_pool.hpp" 
#include "engine/simulator.hpp"

struct Triangle {
    std::string base;
    std::vector<std::string> path;
};

class OrderBookManager;

class TriangleScanner {
public:
    TriangleScanner();

    void setOrderBookManager(OrderBookManager* obm);

    void loadTrianglesFromFile(const std::string& filepath);

    void scanTrianglesForSymbol(const std::string& symbol);

    // Quick 'top-of-book' profit function
    double calculateProfit(const Triangle& tri);

    // Let the user set a minimum profit threshold
    void setMinProfitThreshold(double thresh) { minProfitThreshold_ = thresh; }

    // Let the scanner hold a pointer to the simulator
    void setSimulator(Simulator* sim) { simulator_ = sim; }

private:
    OrderBookManager* obm_{nullptr};

    std::vector<Triangle> triangles_;
    std::unordered_map<std::string, std::vector<int>> symbolToTriangles_;

    double minProfitThreshold_{0.0};
    ThreadPool pool_{4};

    Simulator* simulator_{nullptr};
};

#endif // TRIANGLE_SCANNER_HPP
