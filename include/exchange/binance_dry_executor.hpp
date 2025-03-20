#ifndef BINANCE_DRY_EXECUTOR_HPP
#define BINANCE_DRY_EXECUTOR_HPP

#include "i_exchange_executor.hpp"

/**
 * A dry-run Binance executor that simulates real orders.
 *  - fillRatio: fraction of quantity that actually fills
 *  - baseLatencyMs: “network + engine” latency
 *  - mockPrice: baseline fill price
 *  - slippageBps: basis points of slippage per 1% of the order 
 *
 * Now also randomly simulates network failures and partial fills
 * to demonstrate the new retry logic in Simulator.
 */
class BinanceDryExecutor : public IExchangeExecutor {
public:
    BinanceDryExecutor(double fillRatio=1.0,
                       int baseLatencyMs=150,
                       double mockPrice=28000.0,
                       double slippageBps=50.0);

    OrderResult placeMarketOrder(const std::string& symbol,
                                 OrderSide side,
                                 double quantityBase) override;

    void setMockPrice(double px);
    void setSlippageBps(double bps) { slippageBps_ = bps; }

private:
    double fillRatio_;
    int baseLatencyMs_;
    double mockPrice_;
    double slippageBps_;  // e.g. 50.0 = 0.50% slippage per some volume metric
};

#endif // BINANCE_DRY_EXECUTOR_HPP
