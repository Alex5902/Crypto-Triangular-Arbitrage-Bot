#include "exchange/binance_dry_executor.hpp"
#include <random>
#include <thread>
#include <chrono>
#include <iostream>

BinanceDryExecutor::BinanceDryExecutor(double fillRatio,
                                       int baseLatencyMs,
                                       double mockPrice)
    : fillRatio_(fillRatio)
    , baseLatencyMs_(baseLatencyMs)
    , mockPrice_(mockPrice)
{}

void BinanceDryExecutor::setMockPrice(double px) {
    mockPrice_ = px;
}

OrderResult BinanceDryExecutor::placeMarketOrder(const std::string& symbol,
                                                 OrderSide side,
                                                 double quantityBase)
{
    // measure start time
    auto t0 = std::chrono::high_resolution_clock::now();

    // Simulate network latency
    static std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<> dist(0, baseLatencyMs_);
    int randomExtra = dist(rng); 
    int totalMs = baseLatencyMs_ + randomExtra;
    std::this_thread::sleep_for(std::chrono::milliseconds(totalMs));

    // Build result
    OrderResult result;
    result.success = true;
    result.filledQuantity = quantityBase * fillRatio_;
    result.avgPrice = mockPrice_;

    if (side == OrderSide::BUY) {
        result.costOrProceeds = result.filledQuantity * result.avgPrice;
    } else {
        result.costOrProceeds = result.filledQuantity * result.avgPrice; 
    }
    result.message = "[DRY] " + symbol + " " + 
                     (side==OrderSide::BUY?"BUY ":"SELL ") +
                     std::to_string(result.filledQuantity) +
                     " @ " + std::to_string(result.avgPrice);

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double,std::milli>(t1 - t0).count();

    std::cout << "[DRY] placeMarketOrder symbol=" << symbol
              << " side=" << (side==OrderSide::BUY?"BUY":"SELL")
              << " qty=" << quantityBase << " => fill=" << result.filledQuantity
              << " costOrProceeds=" << result.costOrProceeds
              << " time=" << ms << " ms\n";

    return result;
}
