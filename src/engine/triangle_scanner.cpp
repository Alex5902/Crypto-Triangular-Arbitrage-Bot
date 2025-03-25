#include "engine/triangle_scanner.hpp"
#include "engine/simulator.hpp"  // for calling simulateTradeDepthWithWallet
#include "core/orderbook.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>

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

        for (auto& p : item["path"]) {
            tri.path.push_back(p.get<std::string>());
        }

        // start websockets
        for (const auto& sym : tri.path) {
            std::cout << "[LOADER] Starting WebSocket for symbol: " << sym << "\n";
            obm_->start(sym);
        }

        int idx = (int)triangles_.size();
        triangles_.push_back(tri);
        for (auto& sym : tri.path) {
            symbolToTriangles_[sym].push_back(idx);
        }

        std::cout << "[LOADER] Added triangle: "
                  << tri.path[0] << "->"
                  << tri.path[1] << "->"
                  << tri.path[2] << std::endl;
    }

    std::cout << "Loaded " << triangles_.size()
              << " triangle(s) [no reversed nonsense].\n";
}

void TriangleScanner::scanTrianglesForSymbol(const std::string& symbol) {
    auto t0 = std::chrono::steady_clock::now();
    std::cout << "[CALL] scanTrianglesForSymbol triggered for: " << symbol << "\n";
    if (!obm_) return;

    auto it = symbolToTriangles_.find(symbol);
    if (it == symbolToTriangles_.end()) {
        return;
    }
    const auto& triIndices = it->second;
    std::cout << "[DEBUG] scanning symbol: " << symbol
              << ", #triangles=" << triIndices.size() << std::endl;

    // submit tasks for concurrency
    std::vector<std::future<double>> futures;
    futures.reserve(triIndices.size());
    for (int triIdx : triIndices) {
        futures.push_back(pool_.submit([this, triIdx]() {
            return calculateProfit(triangles_[triIdx]);
        }));
    }

    // gather
    std::vector<double> profits(futures.size());
    for (size_t i = 0; i < futures.size(); i++) {
        double p = futures[i].get();
        profits[i] = p;
    }

    // find best
    double bestProfit = -999.0;
    int bestIndex = -1;
    for (size_t i = 0; i < profits.size(); i++) {
        if (profits[i] == -999) continue;
        if (profits[i] > bestProfit) {
            bestProfit = profits[i];
            bestIndex  = (int)i;
        }
    }

    if (bestProfit > minProfitThreshold_ && bestIndex >= 0) {
        const auto& tri = triangles_[ triIndices[bestIndex] ];
        std::cout << "[BEST ROUTE] "
                  << tri.path[0] << "->"
                  << tri.path[1] << "->"
                  << tri.path[2] << " : "
                  << bestProfit << "%\n";

        if (simulator_) {
            // NEW: do a full USDT estimation first
            auto ob1 = obm_->getOrderBook(tri.path[0]);
            auto ob2 = obm_->getOrderBook(tri.path[1]);
            auto ob3 = obm_->getOrderBook(tri.path[2]);

            double estProfitUSDT = simulator_->estimateTriangleProfitUSDT(tri, ob1, ob2, ob3);
            if (estProfitUSDT < 0.0) {
                std::cout << "[SCAN] Full-triangle check => negative or fail => skipping.\n";
                return;
            }
            // Optionally define your own min USDT threshold
            if (estProfitUSDT < 2.0) {
                // or load from config
                std::cout << "[SCAN] Full-triangle check => " 
                          << estProfitUSDT << " < 2 USDT => skip\n";
                return;
            }

            std::cout << "[SIMULATE] Pre-check => +"
                      << estProfitUSDT << " USDT (est). Doing the real trade.\n";

            // Now we do the actual trade
            simulator_->simulateTradeDepthWithWallet(tri, ob1, ob2, ob3);
            simulator_->printWallet();
        }
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration<double, std::milli>(t1 - t0).count();
    std::cout << "[SCANNER LATENCY] symbol=" << symbol
              << " took " << ms << " ms\n";

    // Log the scan result to CSV
    logScanResult(symbol, (int)triIndices.size(), bestProfit, ms);
}

double TriangleScanner::calculateProfit(const Triangle& tri) {
    if (!obm_) return -999;
    if (tri.path.size() < 3) return -999;

    // Example check (adjust to your liking)
    if (tri.path[0] != "BTCUSDT" && tri.path[0] != "ETHUSDT") {
        return -999;
    }

    auto ob1 = obm_->getOrderBook(tri.path[0]);
    auto ob2 = obm_->getOrderBook(tri.path[1]);
    auto ob3 = obm_->getOrderBook(tri.path[2]);

    if (ob1.bids.empty()|| ob1.asks.empty()||
        ob2.bids.empty()|| ob2.asks.empty()||
        ob3.bids.empty()|| ob3.asks.empty()){
        return -999;
    }

    double bid1 = ob1.bids[0].price;
    double ask1 = ob1.asks[0].price;
    double bid2 = ob2.bids[0].price;
    double ask2 = ob2.asks[0].price;
    double bid3 = ob3.bids[0].price;
    double ask3 = ob3.asks[0].price;

    double fee=0.001;
    double amount=1.0;
    if (tri.path[0]=="BTCUSDT") {
        amount=(amount*bid1)*(1.0 - fee);
        amount=(amount/ask2)*(1.0 - fee);
        amount=(amount*bid3)*(1.0 - fee);
    } else {
        amount=(amount/ask1)*(1.0 - fee);
        amount=(amount*bid2)*(1.0 - fee);
        amount=(amount*bid3)*(1.0 - fee);
    }

    double profitPercent= (amount-1.0)*100.0;
    return profitPercent;
}

void TriangleScanner::scanAllSymbolsConcurrently() {
    std::vector<std::string> allSymbols;
    allSymbols.reserve(symbolToTriangles_.size());
    for (auto& kv : symbolToTriangles_) {
        allSymbols.push_back(kv.first);
    }

    std::vector<std::future<void>> futures;
    futures.reserve(allSymbols.size());
    for (auto& symbol : allSymbols) {
        futures.push_back(pool_.submit([this, symbol]() {
            this->scanTrianglesForSymbol(symbol);
        }));
    }

    for (auto& fut : futures) {
        fut.wait();
    }
}

void TriangleScanner::logScanResult(const std::string& symbol,
                                    int triCount,
                                    double bestProfit,
                                    double latencyMs)
{
    std::lock_guard<std::mutex> lock(scanLogMutex_);
    std::ofstream file("scan_log.csv", std::ios::app);
    if (!file.is_open()) return;

    if (!scanLogHeaderWritten_) {
        file << "timestamp,symbol,triangles_scanned,best_profit,latency_ms\n";
        scanLogHeaderWritten_ = true;
    }

    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);

    file << std::put_time(std::localtime(&now_c), "%F %T") << ","
         << symbol << ","
         << triCount << ","
         << bestProfit << ","
         << latencyMs << "\n";
}
