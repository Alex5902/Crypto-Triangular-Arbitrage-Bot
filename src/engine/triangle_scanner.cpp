#include "engine/triangle_scanner.hpp"
#include "core/orderbook.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <nlohmann/json.hpp>

using json = nlohmann::json;

TriangleScanner::TriangleScanner()
    : pool_(4) // 4 threads, or pick your number
{
}

void TriangleScanner::setOrderBookManager(OrderBookManager* obm) {
    obm_ = obm;
}

void TriangleScanner::loadTrianglesFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if(!file.is_open()) {
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
        int idx = (int)triangles_.size(); // index of this new triangle
        triangles_.push_back(tri);

        // for each symbol in tri, record that tri uses it
        for (auto& symbol : tri.path) {
            symbolToTriangles_[symbol].push_back(idx);
        }
    }
    std::cout << "Loaded " << triangles_.size() << " triangles.\n";
}

/**
 * Called from onMessage(symbol) whenever 'symbol' just got new order book data.
 * We only re-scan the triangles that use 'symbol', in parallel.
 */
void TriangleScanner::scanTrianglesForSymbol(const std::string& symbol) {
    if(!obm_) return;
    auto it = symbolToTriangles_.find(symbol);
    if (it == symbolToTriangles_.end()) {
        // no triangles use that symbol
        return;
    }

    const auto& triIndices = it->second; // vector<int> of triangle indexes

    std::cout << "[DEBUG] scanning symbol: " << symbol 
          << ", #triangles=" << triIndices.size() << std::endl;

    // We'll store futures here
    std::vector<std::future<double>> futures;
    futures.reserve(triIndices.size());

    // For each triangle index, submit a job to the thread pool
    for (int triIdx : triIndices) {
        futures.push_back( pool_.submit([this, triIdx]() {
            return calculateProfit(triangles_[triIdx]);
        }));
    }

    // gather results
    for (size_t i = 0; i < futures.size(); i++) {
        double profit = futures[i].get();
        if (profit == -999) continue; // incomplete data
        if (profit > minProfitThreshold_) {
            // for printing:
            const auto& tri = triangles_[ triIndices[i] ];
            std::cout << "[PARTIAL-RESCAN + THREADPOOL] "
                      << tri.path[0] << "->"
                      << tri.path[1] << "->"
                      << tri.path[2]
                      << " : " << profit << "%\n";
            // TODO: place trades if you want
        }
    }
}

/**
 * The same as before, just reference obm_ for topOfBook.
 */
double TriangleScanner::calculateProfit(const Triangle& tri) {
    if(!obm_) return -999;

    // minimal checks
    if (tri.path.size() < 3) return -999;

    auto b1 = obm_->getTopOfBook(tri.path[0]);
    auto b2 = obm_->getTopOfBook(tri.path[1]);
    auto b3 = obm_->getTopOfBook(tri.path[2]);

    if (b1.bid <= 0 || b1.ask <= 0 ||
        b2.bid <= 0 || b2.ask <= 0 ||
        b3.bid <= 0 || b3.ask <= 0) {
        return -999;
    }

    double fee = 0.001;
    double amount = 1.0;

    if (tri.path[0] == "BTCUSDT") {
        amount = (amount * b1.bid) * (1.0 - fee);
        amount = (amount / b2.ask) * (1.0 - fee);
        amount = (amount * b3.bid) * (1.0 - fee);
    }
    else if(tri.path[0] == "ETHUSDT") {
        amount = (amount / b1.ask) * (1.0 - fee);
        amount = (amount * b2.bid) * (1.0 - fee);
        amount = (amount * b3.bid) * (1.0 - fee);
    }

    double profitPercent = (amount - 1.0) * 100.0;
    return profitPercent;
}
