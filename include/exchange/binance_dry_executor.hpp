#ifndef BINANCE_DRY_EXECUTOR_HPP
#define BINANCE_DRY_EXECUTOR_HPP

#include "i_exchange_executor.hpp"
#include "core/orderbook.hpp"
#include <mutex>
#include <chrono>

class OrderBookManager; // forward declaration if you like

class BinanceDryExecutor : public IExchangeExecutor {
public:
    BinanceDryExecutor(double fillRatio=1.0,
                       int baseLatencyMs=150,
                       double mockPrice=28000.0,
                       double slippageBps=50.0,
                       OrderBookManager* obm=nullptr);

    // From IExchangeExecutor:
    OrderResult placeMarketOrder(const std::string& symbol,
                                 OrderSide side,
                                 double quantityBase) override;

    OrderBookData getOrderBookSnapshot(const std::string& symbol) override;

    // existing:
    void setMockPrice(double px);
    void setSlippageBps(double bps) { slippageBps_ = bps; }

    // Rate-limiter config: same approach as real executor
    void setMaxRequestsPerMinute(int rpm) { maxRequestsPerMinute_ = rpm; }
    void setMaxOrdersPerSecond(int ops)   { maxOrdersPerSec_     = ops; }

private:
    double fillRatio_;
    int baseLatencyMs_;
    double mockPrice_;
    double slippageBps_;

    // pointer to OB manager
    OrderBookManager* obm_;

    // --- Rate limiting / throttler data ---
    int maxRequestsPerMinute_{1200};
    int maxOrdersPerSec_{10};

    static std::mutex throttleMutex_;

    double requestTokens_;
    std::chrono::steady_clock::time_point lastRefillRequests_;

    int orderCountInCurrentSec_;
    std::chrono::steady_clock::time_point currentSecStart_;

private:
    // Rate limiter logic
    void throttleRequest(bool isOrder);
    void refillRequestTokens();
    void resetOrderCounterIfNewSecond();
};

#endif // BINANCE_DRY_EXECUTOR_HPP
