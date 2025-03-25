#ifndef I_EXCHANGE_EXECUTOR_HPP
#define I_EXCHANGE_EXECUTOR_HPP

#include <string>
#include "core/orderbook.hpp"  // so we know OrderBookData
// #include "some_other_headers_if_needed"

enum class OrderSide { BUY, SELL };

struct OrderResult {
    bool success;            
    double filledQuantity;   
    double avgPrice;         
    double costOrProceeds;   
    std::string message;
};

class IExchangeExecutor {
public:
    virtual ~IExchangeExecutor() = default;

    // existing method
    virtual OrderResult placeMarketOrder(
        const std::string& symbol,
        OrderSide side,
        double quantityBase
    ) = 0;

    // NEW method:
    virtual OrderBookData getOrderBookSnapshot(const std::string& symbol) = 0;
};

#endif // I_EXCHANGE_EXECUTOR_HPP
