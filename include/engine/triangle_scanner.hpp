#ifndef TRIANGLE_SCANNER_HPP
#define TRIANGLE_SCANNER_HPP

#include <vector>
#include <string>
#include <unordered_map>
#include <future>

#include "core/thread_pool.hpp"
#include "core/triangle.hpp"  // new separate file for struct Triangle

// Forward-declare, do NOT include "simulator.hpp" here
class Simulator;
class OrderBookManager;

class TriangleScanner {
public:
    TriangleScanner();

    void setOrderBookManager(OrderBookManager* obm);

    void loadTrianglesFromFile(const std::string& filepath);
    void scanTrianglesForSymbol(const std::string& symbol);

    double calculateProfit(const Triangle& tri);

    void setMinProfitThreshold(double thresh) { minProfitThreshold_ = thresh; }
    
    // Just forward declare "class Simulator" above, so you can hold a pointer
    void setSimulator(Simulator* sim) { simulator_ = sim; }

private:
    OrderBookManager* obm_{nullptr};

    std::vector<Triangle> triangles_;
    std::unordered_map<std::string, std::vector<int>> symbolToTriangles_;

    double minProfitThreshold_{0.0};
    ThreadPool pool_{4};

    // pointer only, we do not need the full definition here
    Simulator* simulator_{nullptr};
};

#endif // TRIANGLE_SCANNER_HPP
