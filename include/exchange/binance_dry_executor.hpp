#ifndef BINANCE_DRY_EXECUTOR_HPP
#define BINANCE_DRY_EXECUTOR_HPP

#include "i_exchange_executor.hpp"
#include "core/orderbook.hpp"
// You also need #include "core/orderbook.hpp" or forward-declare.

class OrderBookManager; // forward declaration if you like

class BinanceDryExecutor : public IExchangeExecutor {
public:
    BinanceDryExecutor(double fillRatio=1.0,
                       int baseLatencyMs=150,
                       double mockPrice=28000.0,
                       double slippageBps=50.0,
                       OrderBookManager* obm=nullptr); // <-- pass obm

    // From IExchangeExecutor:
    OrderResult placeMarketOrder(const std::string& symbol,
                                 OrderSide side,
                                 double quantityBase) override;

    // NEW:
    OrderBookData getOrderBookSnapshot(const std::string& symbol) override;

    // existing:
    void setMockPrice(double px);
    void setSlippageBps(double bps) { slippageBps_ = bps; }

private:
    double fillRatio_;
    int baseLatencyMs_;
    double mockPrice_;
    double slippageBps_;

    // pointer to OB manager
    OrderBookManager* obm_;
};

#endif // BINANCE_DRY_EXECUTOR_HPP
