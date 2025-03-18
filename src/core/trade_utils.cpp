// src/core/trade_utils.cpp

#include "core/trade_utils.hpp"

bool TradeUtils::isLiquiditySufficient(const OrderBookEntry& book, double requiredVolume) {
    // Placeholder logic: assume $1000 available liquidity on both sides
    double mid = (book.bid + book.ask) / 2.0;
    double liquidity = mid * 100.0; // simulate $1000 liquidity
    return requiredVolume <= liquidity;
}

double TradeUtils::estimateSlippagePrice(double price, double volume, double liquidity) {
    if (liquidity == 0) return price * 10.0; // bad fallback
    double slippage = volume / liquidity; // crude model
    return price * (1.0 + slippage * 0.1); // simulate small impact
}

double TradeUtils::applyFees(double amount, double feePercent) {
    return amount * (1.0 - feePercent);
}
