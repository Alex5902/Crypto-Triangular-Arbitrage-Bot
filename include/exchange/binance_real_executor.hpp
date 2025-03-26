#ifndef BINANCE_REAL_EXECUTOR_HPP
#define BINANCE_REAL_EXECUTOR_HPP

#include "i_exchange_executor.hpp"
#include "core/orderbook.hpp"
#include <string>
#include <mutex>
#include <chrono>

class OrderBookManager; // forward declare if you use it

/**
 * A real (testnet) Binance executor for spot trades, now with a simple
 * rate-limiting mechanism (token bucket or leaky bucket).
 */
class BinanceRealExecutor : public IExchangeExecutor {
public:
    BinanceRealExecutor(const std::string& apiKey,
                        const std::string& secretKey,
                        const std::string& baseUrl = "https://testnet.binance.vision",
                        OrderBookManager* obm=nullptr);

    OrderResult placeMarketOrder(const std::string& symbol,
                                 OrderSide side,
                                 double quantityBase) override;

    OrderBookData getOrderBookSnapshot(const std::string& symbol) override;

    // optionally, user can set these if your usage differs
    void setMaxRequestsPerMinute(int rpm) { maxRequestsPerMinute_ = rpm; }
    void setMaxOrdersPerSecond(int ops)   { maxOrdersPerSec_     = ops; }

private:
    std::string apiKey_;
    std::string secretKey_;
    std::string baseUrl_;
    OrderBookManager* obm_;

    // helper to create signature, do HTTP post, etc.
    std::string signQueryString(const std::string& query) const;
    std::string httpRequest(const std::string& method,
                            const std::string& endpoint,
                            const std::string& queryString);

    // --- Rate limiting / throttler data ---
    // For instance: 1200 weight/min => 20 requests/sec average.
    // We'll do a simpler version: X requests per minute + Y orders per second
    int maxRequestsPerMinute_{1200}; 
    int maxOrdersPerSec_{10}; // e.g. 10 new orders per second

    // We'll store the last times we've made requests and orders
    // plus counters to ensure we don't exceed short-term bursts
    static std::mutex throttleMutex_;

    // token bucket approach for general requests
    double requestTokens_{ (double)maxRequestsPerMinute_ }; 
    std::chrono::steady_clock::time_point lastRefillRequests_;

    // short-burst order count
    int orderCountInCurrentSec_{0};
    std::chrono::steady_clock::time_point currentSecStart_;

private:
    // We'll unify the logic in this method:
    void throttleRequest(bool isOrder);
    void refillRequestTokens();
    void resetOrderCounterIfNewSecond();
};

#endif // BINANCE_REAL_EXECUTOR_HPP
