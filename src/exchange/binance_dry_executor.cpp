#include "exchange/binance_dry_executor.hpp"
#include <thread>
#include <chrono>
#include <cmath>
#include <iostream>

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
    // Simulate latency
    std::this_thread::sleep_for(std::chrono::milliseconds(baseLatencyMs_));

    OrderResult res;
    res.success = true;

    // The fill quantity
    res.filledQuantity = quantityBase * fillRatio_;

    // Now do a simple slippage calc based on side + quantity
    // Example: if we want to buy a big chunk, we pay more than mockPrice
    // slippageBps_ is arbitrary. We can do: newPrice = mockPrice_ * (1 + slipRatio)
    // slipRatio might scale with quantity, etc.
    double slipRatio = 0.0;
    // Example function: slip = 0.01% per 1 base unit
    // or you can do bigger or smaller:
    slipRatio = (quantityBase * slippageBps_) / 10000.0; // bps means /10000

    double sideFactor = (side == OrderSide::BUY ? +1.0 : -1.0);
    double adjustedPrice = mockPrice_ * (1.0 + sideFactor * slipRatio);

    res.avgPrice = adjustedPrice;

    // cost or proceeds
    res.costOrProceeds = res.filledQuantity * res.avgPrice;

    // Debug
    std::cout << "[DRY] side=" << (side==OrderSide::BUY?"BUY":"SELL")
              << " quantityBase=" << quantityBase
              << " fillRatio=" << fillRatio_
              << " finalQty=" << res.filledQuantity
              << " slipRatio=" << slipRatio
              << " basePrice=" << mockPrice_
              << " adjustedPrice=" << adjustedPrice
              << std::endl;

    return res;
}

void BinanceDryExecutor::setMockPrice(double px) {
    mockPrice_ = px;
}
