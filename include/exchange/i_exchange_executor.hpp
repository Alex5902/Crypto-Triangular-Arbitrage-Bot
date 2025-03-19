#ifndef I_EXCHANGE_EXECUTOR_HPP
#define I_EXCHANGE_EXECUTOR_HPP

#include <string>

enum class OrderSide { BUY, SELL };

struct OrderResult {
    bool success;            // did it fill or not
    double filledQuantity;   // how many base units were filled
    double avgPrice;         // average fill price
    double costOrProceeds;   // cost if buy, proceeds if sell
    std::string message;
};

class IExchangeExecutor {
public:
    virtual ~IExchangeExecutor() = default;

    /**
     * placeMarketOrder:
     * @param symbol: e.g. "BTCUSDT"
     * @param side: BUY or SELL
     * @param quantityBase: how many base units (e.g. 0.01 BTC)
     */
    virtual OrderResult placeMarketOrder(
        const std::string& symbol,
        OrderSide side,
        double quantityBase
    ) = 0;
};

#endif // I_EXCHANGE_EXECUTOR_HPP
