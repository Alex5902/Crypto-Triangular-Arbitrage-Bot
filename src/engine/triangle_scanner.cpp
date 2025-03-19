#include "engine/triangle_scanner.hpp"
#include "engine/simulator.hpp" 
#include "core/orderbook.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>    // for std::reverse if needed
#include <nlohmann/json.hpp>

using json = nlohmann::json;

TriangleScanner::TriangleScanner()
    : pool_(4)
{
}

void TriangleScanner::setOrderBookManager(OrderBookManager* obm) {
    obm_ = obm;
}

void TriangleScanner::loadTrianglesFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Cannot open triangle file " << filepath << std::endl;
        return;
    }

    json j;
    file >> j;
    for (auto& item : j) {
        Triangle tri;
        tri.base = item["base"].get<std::string>();

        // parse the "path"
        for (auto& p : item["path"]) {
            tri.path.push_back(p.get<std::string>());
        }

        // Start websockets for each symbol in the path
        for (const auto& sym : tri.path) {
            std::cout << "[LOADER] Starting WebSocket for symbol: " << sym << "\n";
            obm_->start(sym);
        }

        // Add ONLY the forward triangle to our list
        int idx = (int)triangles_.size();
        triangles_.push_back(tri);

        for (auto& sym : tri.path) {
            symbolToTriangles_[sym].push_back(idx);
        }

        std::cout << "[LOADER] Added triangle: "
                  << tri.path[0] << "->" << tri.path[1] << "->" << tri.path[2]
                  << std::endl;
    }

    std::cout << "Loaded " << triangles_.size() << " triangle(s) [no reversed nonsense].\n";
}

void TriangleScanner::scanTrianglesForSymbol(const std::string& symbol) {
    auto t0 = std::chrono::steady_clock::now();
    std::cout << "[CALL] scanTrianglesForSymbol triggered for: " << symbol << "\n";
    if (!obm_) return;

    auto it = symbolToTriangles_.find(symbol);
    if (it == symbolToTriangles_.end()) {
        // no triangles for this symbol
        return;
    }
    const auto& triIndices = it->second;
    std::cout << "[DEBUG] scanning symbol: " << symbol
              << ", #triangles=" << triIndices.size() << std::endl;

    // 1) Submit tasks in parallel
    std::vector<std::future<double>> futures;
    futures.reserve(triIndices.size());
    for (int triIdx : triIndices) {
        futures.push_back(pool_.submit([this, triIdx]() {
            return calculateProfit(triangles_[triIdx]);
        }));
    }

    // 2) Gather results
    std::vector<double> profits(futures.size());
    for (size_t i = 0; i < futures.size(); i++) {
        double p = futures[i].get();
        profits[i] = p;
    }

    // 3) Find best route
    double bestProfit = -999.0;
    int bestIndex = -1;
    for (size_t i = 0; i < profits.size(); i++) {
        if (profits[i] == -999) continue;
        if (profits[i] > bestProfit) {
            bestProfit = profits[i];
            bestIndex  = (int)i;
        }
    }

    // 4) If bestProfit > threshold => do exactly one trade
    if (bestProfit > minProfitThreshold_ && bestIndex >= 0) {
        const auto& tri = triangles_[ triIndices[bestIndex] ];
        std::cout << "[BEST ROUTE] "
                  << tri.path[0] << "->"
                  << tri.path[1] << "->"
                  << tri.path[2]
                  << " : " << bestProfit << "%\n";

        if (simulator_) {
            // We now call the simulator with the *entire order book* for each pair
            auto ob1 = obm_->getOrderBook(tri.path[0]);
            auto ob2 = obm_->getOrderBook(tri.path[1]);
            auto ob3 = obm_->getOrderBook(tri.path[2]);

            std::cout << "[SIMULATE] ... with profit: " << bestProfit << "\n";
            simulator_->simulateTradeDepthWithWallet(tri, ob1, ob2, ob3);

            simulator_->printWallet();
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration<double,std::milli>(t1 - t0).count();
    std::cout << "[SCANNER LATENCY] symbol=" << symbol << " took " << ms << " ms\n";
}

double TriangleScanner::calculateProfit(const Triangle& tri) {
    if (!obm_) return -999;
    if (tri.path.size() < 3) return -999;

    // skip if start is not "BTCUSDT" or "ETHUSDT"
    if (tri.path[0] != "BTCUSDT" && tri.path[0] != "ETHUSDT") {
        return -999;
    }

    // We'll do a "fake" partial fill logic or a simplified approach to measure profit
    // But let's keep it simple here – or you can do your advanced approach in simulator:
    auto ob1 = obm_->getOrderBook(tri.path[0]);
    auto ob2 = obm_->getOrderBook(tri.path[1]);
    auto ob3 = obm_->getOrderBook(tri.path[2]);

    // If any depth is empty, skip
    if (ob1.bids.empty() || ob1.asks.empty() ||
        ob2.bids.empty() || ob2.asks.empty() ||
        ob3.bids.empty() || ob3.asks.empty()) {
        return -999;
    }

    // We'll just approximate "top-of-book" from the first level
    double bid1 = ob1.bids[0].price;
    double ask1 = ob1.asks[0].price;
    double bid2 = ob2.bids[0].price;
    double ask2 = ob2.asks[0].price;
    double bid3 = ob3.bids[0].price;
    double ask3 = ob3.asks[0].price;

    double fee = 0.001;
    double amount = 1.0;

    if (tri.path[0] == "BTCUSDT") {
        amount = (amount * bid1) * (1.0 - fee);
        amount = (amount / ask2) * (1.0 - fee);
        amount = (amount * bid3) * (1.0 - fee);
    }
    else if (tri.path[0] == "ETHUSDT") {
        amount = (amount / ask1) * (1.0 - fee);
        amount = (amount * bid2) * (1.0 - fee);
        amount = (amount * bid3) * (1.0 - fee);
    }

    double profitPercent = (amount - 1.0) * 100.0;
    return profitPercent;
}
