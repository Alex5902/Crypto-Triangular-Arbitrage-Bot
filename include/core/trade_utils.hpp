#ifndef TRADE_UTILS_HPP
#define TRADE_UTILS_HPP

#include "core/orderbook.hpp"

namespace TradeUtils {
    bool isLiquiditySufficient(const OrderBookEntry& book, double requiredVolume);
    double estimateSlippagePrice(double price, double volume, double liquidity);
    double applyFees(double amount, double feePercent);
}

#endif // TRADE_UTILS_HPP
