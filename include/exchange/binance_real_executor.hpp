#ifndef BINANCE_REAL_EXECUTOR_HPP
#define BINANCE_REAL_EXECUTOR_HPP

#include "i_exchange_executor.hpp"
#include "core/orderbook.hpp"
#include <string>

class OrderBookManager; // forward declare if you use it

/**
 * A real (testnet) Binance executor for spot trades.
 */
class BinanceRealExecutor : public IExchangeExecutor {
public:
    BinanceRealExecutor(const std::string& apiKey,
                        const std::string& secretKey,
                        const std::string& baseUrl = "https://testnet.binance.vision",
                        OrderBookManager* obm=nullptr); // new param

    OrderResult placeMarketOrder(const std::string& symbol,
                                 OrderSide side,
                                 double quantityBase) override;

    // NEW:
    OrderBookData getOrderBookSnapshot(const std::string& symbol) override;

private:
    std::string apiKey_;
    std::string secretKey_;
    std::string baseUrl_;
    // optionally store
    OrderBookManager* obm_; // pointer to your orderbook manager

    // helper to create signature, do HTTP post, etc.
    std::string signQueryString(const std::string& query) const;
    std::string httpRequest(const std::string& method,
                            const std::string& endpoint,
                            const std::string& queryString);
};

#endif // BINANCE_REAL_EXECUTOR_HPP
