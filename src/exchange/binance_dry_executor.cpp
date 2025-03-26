#include "exchange/binance_dry_executor.hpp"
#include <thread>
#include <chrono>
#include <cmath>
#include <iostream>
#include <random> // for random_device, mt19937, uniform_real_distribution
#include "core/orderbook.hpp" // so we can return OrderBookData

// initialize static
std::mutex BinanceDryExecutor::throttleMutex_{};

BinanceDryExecutor::BinanceDryExecutor(double fillRatio,
                                       int baseLatencyMs,
                                       double mockPrice,
                                       double slippageBps,
                                       OrderBookManager* obm)
  : fillRatio_(fillRatio)
  , baseLatencyMs_(baseLatencyMs)
  , mockPrice_(mockPrice)
  , slippageBps_(slippageBps)
  , obm_(obm)
{
    // Initialize rate limit state
    requestTokens_         = (double)maxRequestsPerMinute_;
    lastRefillRequests_    = std::chrono::steady_clock::now();

    currentSecStart_       = std::chrono::steady_clock::now();
    orderCountInCurrentSec_= 0;
}

OrderResult BinanceDryExecutor::placeMarketOrder(const std::string& symbol,
                                                 OrderSide side,
                                                 double quantityBase)
{
    // Rate-limit this call as an "order"
    throttleRequest(/*isOrder=*/true);

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

    // Partial fill logic
    {
        static thread_local std::mt19937 rng( std::random_device{}() );
        std::uniform_real_distribution<double> dist(0.5, 1.0);
        double partialFactor = dist(rng);
        res.filledQuantity = quantityBase * fillRatio_ * partialFactor;
    }

    // Simple slippage
    double slipRatio  = (quantityBase * slippageBps_) / 10000.0;
    double sideFactor = (side == OrderSide::BUY ? +1.0 : -1.0);
    double adjustedPrice = mockPrice_ * (1.0 + sideFactor * slipRatio);

    res.avgPrice       = adjustedPrice;
    res.costOrProceeds = res.filledQuantity * res.avgPrice;

    // Debug
    std::cout << "[DRY] symbol=" << symbol
              << " side=" << (side==OrderSide::BUY ? "BUY" : "SELL")
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

// Throttled read
OrderBookData BinanceDryExecutor::getOrderBookSnapshot(const std::string& symbol)
{
    // Rate-limit as a normal request (not an "order")
    throttleRequest(/*isOrder=*/false);

    if (!obm_) {
        std::cerr << "[DRY] No OrderBookManager provided => returning empty OB\n";
        return OrderBookData{}; // empty
    }
    return obm_->getOrderBook(symbol);
}

// existing helper
void BinanceDryExecutor::setMockPrice(double px) {
    mockPrice_ = px;
}

/**
 * Implement the same approach as real executor:
 * a simple token-bucket for requests + short-burst limit for orders.
 */
void BinanceDryExecutor::throttleRequest(bool isOrder)
{
    std::lock_guard<std::mutex> lg(throttleMutex_);

    // refill request tokens
    refillRequestTokens();

    // if it's an order, check short-burst
    if(isOrder){
        resetOrderCounterIfNewSecond();
        while(orderCountInCurrentSec_ >= maxOrdersPerSec_){
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            resetOrderCounterIfNewSecond();
        }
        orderCountInCurrentSec_++;
    }

    // consume 1 request token
    while(requestTokens_ < 1.0){
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        refillRequestTokens();
    }
    requestTokens_ -= 1.0;
}

void BinanceDryExecutor::refillRequestTokens()
{
    auto now = std::chrono::steady_clock::now();
    double secondsElapsed = std::chrono::duration<double>(now - lastRefillRequests_).count();

    double tokensPerSecond = (double)maxRequestsPerMinute_ / 60.0;
    double tokensToAdd     = tokensPerSecond * secondsElapsed;

    if(tokensToAdd >= 1.0){
        requestTokens_ = std::min(
            (double)maxRequestsPerMinute_,
            requestTokens_ + tokensToAdd
        );
        lastRefillRequests_ = now;
    }
}

void BinanceDryExecutor::resetOrderCounterIfNewSecond()
{
    auto now = std::chrono::steady_clock::now();
    double msElapsed = std::chrono::duration<double,std::milli>(now - currentSecStart_).count();
    if(msElapsed >= 1000.0){
        currentSecStart_ = now;
        orderCountInCurrentSec_ = 0;
    }
}
