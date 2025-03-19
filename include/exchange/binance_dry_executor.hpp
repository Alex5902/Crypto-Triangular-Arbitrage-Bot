#ifndef BINANCE_DRY_EXECUTOR_HPP
#define BINANCE_DRY_EXECUTOR_HPP

#include "i_exchange_executor.hpp"

/**
 * A dry-run Binance executor that simulates real orders.
 *  - baseLatencyMs: the baseline “network + matching engine” latency
 *  - fillRatio: fraction of quantity that actually fills
 *  - mockPrice: the fill price used for cost/proceeds
 */
class BinanceDryExecutor : public IExchangeExecutor {
public:
    BinanceDryExecutor(double fillRatio=1.0,
                       int baseLatencyMs=150,
                       double mockPrice=28000.0);

    OrderResult placeMarketOrder(const std::string& symbol,
                                 OrderSide side,
                                 double quantityBase) override;

    void setMockPrice(double px);

private:
    double fillRatio_;
    int baseLatencyMs_;
    double mockPrice_;
};

#endif // BINANCE_DRY_EXECUTOR_HPP
