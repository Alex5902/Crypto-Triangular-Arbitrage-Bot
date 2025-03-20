#ifndef BINANCE_REAL_EXECUTOR_HPP
#define BINANCE_REAL_EXECUTOR_HPP

#include "i_exchange_executor.hpp"
#include <string>

/**
 * A real (testnet) Binance executor for spot trades.
 *  - Connects to the Binance Spot Testnet
 *  - Uses API key/secret for signing
 *  - placeMarketOrder => calls /api/v3/order?...
 * 
 * This example uses raw HTTP via libcurl or a similar approach.
 */
class BinanceRealExecutor : public IExchangeExecutor {
public:
    /**
     * @param apiKey your testnet API key
     * @param secretKey your testnet secret
     * @param baseUrl e.g. "https://testnet.binance.vision"
     */
    BinanceRealExecutor(const std::string& apiKey,
                        const std::string& secretKey,
                        const std::string& baseUrl = "https://testnet.binance.vision");

    OrderResult placeMarketOrder(const std::string& symbol,
                                 OrderSide side,
                                 double quantityBase) override;

    // Optionally you can add fetchOrderStatus, cancelOrder, etc.

private:
    std::string apiKey_;
    std::string secretKey_;
    std::string baseUrl_;

    // helper to create signature, do HTTP post, etc.
    std::string signQueryString(const std::string& query) const;
    std::string httpRequest(const std::string& method,
                            const std::string& endpoint,
                            const std::string& queryString);
};

#endif // BINANCE_REAL_EXECUTOR_HPP
