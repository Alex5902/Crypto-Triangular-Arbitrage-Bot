#include "exchange/binance_dry_executor.hpp"
#include <thread>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random> // for random_device, mt19937, uniform_real_distribution

BinanceDryExecutor::BinanceDryExecutor(double fillRatio,
                                       int baseLatencyMs,
                                       double mockPrice,
                                       double slippageBps)
  : fillRatio_(fillRatio)
  , baseLatencyMs_(baseLatencyMs)
  , mockPrice_(mockPrice)
  , slippageBps_(slippageBps)
{}

OrderResult BinanceDryExecutor::placeMarketOrder(const std::string& symbol,
                                                 OrderSide side,
                                                 double quantityBase)
{
    // Simulate network + engine latency
    std::this_thread::sleep_for(std::chrono::milliseconds(baseLatencyMs_));

    OrderResult res;
    res.success = true;

    // Introduce random chance of "transient network failure"
    {
        static thread_local std::mt19937 rng( std::random_device{}() );
        std::uniform_real_distribution<double> dist01(0.0,1.0);

        double r = dist01(rng);
        // e.g. 10% chance we fail completely
        if (r < 0.10) {
            std::cout << "[DRY] Simulating transient network error.\n";
            res.success = false;
            return res;
        }
    }

    // We do partial fill logic by random factor around fillRatio_
    // e.g. if fillRatio_ = 1.0 but random partial is 70% => 0.7
    {
        static thread_local std::mt19937 rng( std::random_device{}() );
        std::uniform_real_distribution<double> dist(0.5, 1.0); 
        double partialFactor = dist(rng); // random in [0.5..1.0]
        res.filledQuantity = quantityBase * fillRatio_ * partialFactor;
    }

    // Now do a simple slippage calc based on side + quantity
    double slipRatio = 0.0;
    // Example function: slip = slippageBps_ * quantity / 10000
    slipRatio = (quantityBase * slippageBps_) / 10000.0;

    double sideFactor = (side == OrderSide::BUY ? +1.0 : -1.0);
    double adjustedPrice = mockPrice_ * (1.0 + sideFactor * slipRatio);

    res.avgPrice = adjustedPrice;

    // cost or proceeds
    res.costOrProceeds = res.filledQuantity * res.avgPrice;

    // Debug
    std::cout << "[DRY] symbol=" << symbol
              << " side=" << (side==OrderSide::BUY?"BUY":"SELL")
              << " qtyReq=" << quantityBase
              << " finalQty=" << res.filledQuantity
              << " fillRatioParam=" << fillRatio_
              << " slipRatio=" << slipRatio
              << " basePrice=" << mockPrice_
              << " adjustedPrice=" << adjustedPrice
              << " success=" << (res.success?"true":"false")
              << std::endl;

    return res;
}

void BinanceDryExecutor::setMockPrice(double px) {
    mockPrice_ = px;
}
