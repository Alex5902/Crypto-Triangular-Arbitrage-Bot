#ifndef I_EXCHANGE_EXECUTOR_HPP
#define I_EXCHANGE_EXECUTOR_HPP

#include <string>
#include "core/orderbook.hpp"  // so we know OrderBookData

enum class OrderSide { BUY, SELL };

struct OrderResult {
    bool success;            
    double filledQuantity;   // e.g. how much base was filled
    double avgPrice;         // average fill price
    double costOrProceeds;   // total quote used/received
    std::string message;
};

class IExchangeExecutor {
public:
    virtual ~IExchangeExecutor() = default;

    // Place a market order for `quantityBase` units of base asset
    virtual OrderResult placeMarketOrder(
        const std::string& symbol,
        OrderSide side,
        double quantityBase
    ) = 0;

    // get local snapshot or fetch from an external endpoint
    virtual OrderBookData getOrderBookSnapshot(const std::string& symbol) = 0;
};

#endif // I_EXCHANGE_EXECUTOR_HPP
