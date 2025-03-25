#include "engine/simulator.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cmath>
#include <thread>
#include <algorithm>

// For JSON
#include <nlohmann/json.hpp>
using json = nlohmann::json;

// Example known quotes. parseSymbol will detect them as suffix.
static std::vector<std::string> knownQuotes = {
    "USDT","BTC","ETH","BNB","BUSD","USDC"
};

// parseSymbol => e.g. "BTCUSDT" => {"BTC","USDT"}
std::pair<std::string,std::string> parseSymbol(const std::string& pair) {
    for (const auto& q : knownQuotes) {
        if (pair.size() > q.size()) {
            size_t pos = pair.rfind(q);
            if (pos != std::string::npos && (pos + q.size()) == pair.size()) {
                std::string base = pair.substr(0, pos);
                return { base, q };
            }
        }
    }
    return { pair, "UNKNOWN" };
}

std::map<std::string, std::mutex> Simulator::assetLocks_;

Simulator::Simulator(const std::string& logFileName,
                     double feePercent,
                     double slippageTolerance,
                     double volumeLimit,
                     double minFillRatio,
                     Wallet* sharedWallet,
                     IExchangeExecutor* executor,
                     double minProfitUSDT)
  : logFileName_(logFileName)
  , feePercent_(feePercent)
  , slippageTolerance_(slippageTolerance)
  , volumeLimit_(volumeLimit)
  , minFillRatio_(minFillRatio)
  , wallet_(sharedWallet)
  , executor_(executor)
  , minProfitUSDT_(minProfitUSDT)
  , liveMode_(false)
{
    // Initialize static assetLocks_ once
    if (assetLocks_.empty()) {
        assetLocks_.try_emplace("BTC");
        assetLocks_.try_emplace("ETH");
        assetLocks_.try_emplace("USDT");
        assetLocks_.try_emplace("BNB");
        assetLocks_.try_emplace("BUSD");
        assetLocks_.try_emplace("USDC");
    }

    // Start or append the sim_log
    std::ofstream file(logFileName_, std::ios::app);
    if (file.is_open()) {
        file << "timestamp,path,start_val,end_val,profit_percent\n";
    }

    //  NEW: load symbol filters from config file
    loadSymbolFilters("config/symbol_filters.json");
}

/**
 * Load minNotional, minQty per symbol from JSON file.
 */
void Simulator::loadSymbolFilters(const std::string& path)
{
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[SIM] Could not open symbol filters file: " << path << "\n";
        std::cerr << "[SIM] Orders may fail if below exchange limits.\n";
        return;
    }

    try {
        json j;
        f >> j;
        for (auto it = j.begin(); it != j.end(); ++it) {
            std::string sym = it.key();  // e.g. "BTCUSDT"
            auto obj        = it.value();
            SymbolFilter sf;
            sf.minNotional  = obj.value("minNotional", 10.0);
            sf.minQty       = obj.value("minQty", 0.0001);
            symbolFilters_[sym] = sf;
        }
        std::cout << "[SIM] Loaded " << symbolFilters_.size()
                  << " symbol filters from " << path << "\n";
    } catch (std::exception& e) {
        std::cerr << "[SIM] Error parsing symbol_filters.json: " << e.what() << "\n";
    }
}

/**
 * Return true if "quantityBase * priceEstimate >= minNotional"
 * and "quantityBase >= minQty" for the given symbol filter.
 */
bool Simulator::passesExchangeFilters(const std::string& symbol,
                                      double quantityBase,
                                      double priceEstimate)
{
    if (symbolFilters_.count(symbol) == 0) {
        // no filter data => skip check or use fallback
        // We'll do a default check: minNotional=10, minQty=0.0001
        double notional = quantityBase * priceEstimate;
        if (notional < 10.0 || quantityBase < 0.0001) {
            std::cout << "[FILTER] " << symbol
                      << ": below default minNotional=10 or minQty=0.0001\n";
            return false;
        }
        return true;
    }

    auto& filt = symbolFilters_[symbol];
    double notional = quantityBase * priceEstimate;

    // Check minQty
    if (quantityBase < filt.minQty) {
        std::cout << "[FILTER] " << symbol << ": quantityBase="
                  << quantityBase << " < minQty=" << filt.minQty << "\n";
        return false;
    }
    // Check minNotional
    if (notional < filt.minNotional) {
        std::cout << "[FILTER] " << symbol << ": notional="
                  << notional << " < minNotional=" << filt.minNotional << "\n";
        return false;
    }
    return true;
}

/**
 * Main 3-leg function:
 *  1) Calls estimateTriangleProfitUSDT(...) to check if final USDT - start USDT is >= minProfitUSDT_.
 *  2) If negative or too small => skip.
 *  3) Otherwise, lock assets, do actual legs (live or sim).
 */
bool Simulator::simulateTradeDepthWithWallet(const Triangle& tri,
                                             const OrderBookData& ob1,
                                             const OrderBookData& ob2,
                                             const OrderBookData& ob3)
{
    // 1) Measure your starting total USDT “mark-to-market”
    double b1 = (ob1.bids.empty() ? 0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty() ? 0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty() ? 0.0 : ob3.bids[0].price);

    double oldValUSDT = wallet_->getFreeBalance("BTC") * b1
                      + wallet_->getFreeBalance("ETH") * b2
                      + wallet_->getFreeBalance("USDT");

    // 2) Estimate final USDT if all 3 legs fill successfully
    double estProfitUSDT = estimateTriangleProfitUSDT(tri, ob1, ob2, ob3);
    if (estProfitUSDT < 0.0) {
        std::cout << "[SIM] Pre-check => route not profitable or fill failed. Skipping.\n";
        return false;
    }
    if (estProfitUSDT < minProfitUSDT_) {
        std::cout << "[SIM] Pre-check => estProfit=" << estProfitUSDT
                  << " < min=" << minProfitUSDT_ << " USDT => skipping.\n";
        return false;
    }

    // Passed the pre-check => proceed with real (or sim) trades
    // Lock all assets used by tri.path to avoid concurrency conflicts
    std::vector<std::string> allAssets;
    for (auto& p : tri.path) {
        auto pairAssets = getAssetsForPair(p);
        for (auto& a : pairAssets) {
            if (std::find(allAssets.begin(), allAssets.end(), a) == allAssets.end()) {
                allAssets.push_back(a);
            }
        }
    }
    std::sort(allAssets.begin(), allAssets.end());

    std::vector<std::unique_lock<std::mutex>> lockGuards;
    lockGuards.reserve(allAssets.size());
    for (auto& asset : allAssets) {
        lockGuards.emplace_back(assetLocks_[asset]);
    }

    // 3) Begin transaction for the actual legs
    auto tx = wallet_->beginTransaction();

    // Leg 1
    if (!doLeg(tx, tri.path[0], ob1)) {
        std::cout << "[SIM] Leg1 fail, rollback.\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    // Leg 2
    if (!doLeg(tx, tri.path[1], ob2)) {
        std::cout << "[SIM] Leg2 fail, rollback.\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    // Leg 3
    if (!doLeg(tx, tri.path[2], ob3)) {
        std::cout << "[SIM] Leg3 fail, rollback.\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }

    // All 3 legs succeeded => commit
    wallet_->commitTransaction(tx);

    // Measure final total USDT
    double newValUSDT = wallet_->getFreeBalance("BTC") * b3
                      + wallet_->getFreeBalance("ETH") * b2
                      + wallet_->getFreeBalance("USDT");
    double absoluteProfit = (newValUSDT - oldValUSDT);
    double profitPercent  = 0.0;
    if (oldValUSDT > 0.0) {
        profitPercent = (absoluteProfit / oldValUSDT) * 100.0;
    }

    // Log the 3-leg path and profit
    std::stringstream ps;
    for (size_t i = 0; i < tri.path.size(); i++) {
        if (i > 0) ps << "->";
        ps << tri.path[i];
    }
    logTrade(ps.str(), oldValUSDT, newValUSDT, profitPercent);

    // Update counters
    if (absoluteProfit > -1e-14) {
        ++totalTrades_;
        cumulativeProfit_ += absoluteProfit;
    }

    std::cout << "[SIM] Traded triangle: " << ps.str()
              << " oldVal=" << oldValUSDT
              << " newVal=" << newValUSDT
              << " profit=" << profitPercent << "%\n";
    return true;
}

/**
 * estimateTriangleProfitUSDT => does a “dry run” of all 3 legs in orderbook depth,
 * applying fees & slippage, returning: (final USDT - start USDT).
 * If anything fails => returns negative.
 */
double Simulator::estimateTriangleProfitUSDT(const Triangle& tri,
                                             const OrderBookData& ob1,
                                             const OrderBookData& ob2,
                                             const OrderBookData& ob3)
{
    // 1) Start USDT mark-to-market from real wallet
    double b1 = (ob1.bids.empty() ? 0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty() ? 0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty() ? 0.0 : ob3.bids[0].price);

    double startValUSDT = wallet_->getFreeBalance("BTC") * b1
                        + wallet_->getFreeBalance("ETH") * b2
                        + wallet_->getFreeBalance("USDT");

    // 2) Create a "fake" wallet for sim
    double fakeBTC  = wallet_->getFreeBalance("BTC");
    double fakeETH  = wallet_->getFreeBalance("ETH");
    double fakeUSDT = wallet_->getFreeBalance("USDT");

    // Helper lambda to simulate one leg in the fake wallet
    auto simulateLeg = [&](const std::string& pairName, const OrderBookData& ob) -> bool
    {
        auto [baseAsset, quoteAsset] = parseSymbol(pairName);
        if (quoteAsset == "UNKNOWN") {
            return false;
        }
        // Decide if we are "selling base for quote" or "buying base with quote"
        bool isSell = (quoteAsset=="USDT" || quoteAsset=="BTC" || quoteAsset=="BUSD" || quoteAsset=="ETH");

        // We'll pick a rough "bestPx" to estimate notional. 
        // For a sell, bestPx = top bid; for a buy, bestPx = top ask.
        double bestPx = 0.0;
        if (isSell) {
            if (!ob.bids.empty()) bestPx = ob.bids[0].price;
        } else {
            if (!ob.asks.empty()) bestPx = ob.asks[0].price;
        }
        if (bestPx <= 0.0) {
            return false;
        }

        // (Now check how much we can spend/sell)
        double freeAmt = 0.0;
        if (isSell) {
            if      (baseAsset == "BTC") freeAmt = fakeBTC;
            else if (baseAsset == "ETH") freeAmt = fakeETH;
            else if (baseAsset == "BNB") {}
            else if (baseAsset == "BUSD") {}
            else if (baseAsset == "USDC") {}
            if (freeAmt <= 0.0) return false;

            double desiredQtyBase = std::min(freeAmt, volumeLimit_);
            if (desiredQtyBase <= 0.0) return false;

            // ***CHECK FILTERS***
            // We'll do a quick notional guess = desiredQtyBase * bestPx
            if (!passesExchangeFilters(pairName, desiredQtyBase, bestPx)) {
                return false;
            }

            // Depth fill simulation ...
            // (unchanged from your code)
            const auto& levels = ob.bids;
            if (levels.empty()) return false;

            double filledQty = 0.0, cost = 0.0;
            double remaining = desiredQtyBase;
            for (auto& lvl : levels) {
                double tradeQty  = std::min(remaining, lvl.quantity);
                double tradeCost = tradeQty * lvl.price;
                filledQty  += tradeQty;
                cost       += tradeCost;
                remaining  -= tradeQty;
                if (remaining <= 1e-12) break;
            }
            if (filledQty <= 1e-12) return false;

            double fillRatio = filledQty / desiredQtyBase;
            if (fillRatio < minFillRatio_) return false;

            // Slippage check
            double avgPx = cost / filledQty;
            double slip  = std::fabs(avgPx - bestPx) / bestPx;
            if (slip > slippageTolerance_) return false;

            // Fee
            double netProceeds = cost * (1.0 - feePercent_);

            // Update fake wallet
            if      (baseAsset == "BTC") fakeBTC -= filledQty;
            else if (baseAsset == "ETH") fakeETH -= filledQty;

            if      (quoteAsset == "USDT") fakeUSDT += netProceeds;
            else if (quoteAsset == "BTC")  fakeBTC  += netProceeds;
            else if (quoteAsset == "ETH")  fakeETH  += netProceeds;
        }
        else {
            // We are buying baseAsset with quoteAsset
            if      (quoteAsset == "USDT") freeAmt = fakeUSDT;
            else if (quoteAsset == "BTC")  freeAmt = fakeBTC;
            else if (quoteAsset == "ETH")  freeAmt = fakeETH;
            if (freeAmt <= 0.0) return false;

            double maxSpend = volumeLimit_ * bestPx;
            double spend = std::min(freeAmt, maxSpend);
            if (spend <= 0.0) return false;

            double desiredQtyBase = (bestPx > 0.0 ? spend / bestPx : 0.0);
            if (desiredQtyBase <= 1e-12) return false;

            // ***CHECK FILTERS***
            // We'll do a quick notional guess = desiredQtyBase * bestPx
            if (!passesExchangeFilters(pairName, desiredQtyBase, bestPx)) {
                return false;
            }

            const auto& levels = ob.asks;
            if (levels.empty()) return false;

            double filledQty=0.0, cost=0.0;
            double remaining= desiredQtyBase;
            for (auto& lvl : levels) {
                double tradeQty  = std::min(remaining, lvl.quantity);
                double tradeCost = tradeQty * lvl.price;
                filledQty += tradeQty;
                cost      += tradeCost;
                remaining -= tradeQty;
                if (remaining <= 1e-12) break;
            }
            if (filledQty <= 1e-12) return false;

            double fillRatio = filledQty / desiredQtyBase;
            if (fillRatio < minFillRatio_) return false;

            double avgPx = cost / filledQty;
            double slip  = std::fabs(avgPx - bestPx) / bestPx;
            if (slip > slippageTolerance_) return false;

            // Fee
            double netCost = cost * (1.0 + feePercent_);

            // Update wallet
            if      (quoteAsset == "USDT") fakeUSDT -= netCost;
            else if (quoteAsset == "BTC")  fakeBTC  -= netCost;
            else if (quoteAsset == "ETH")  fakeETH  -= netCost;

            if      (baseAsset == "BTC")   fakeBTC  += filledQty;
            else if (baseAsset == "ETH")   fakeETH  += filledQty;
        }
        return true;
    };

    // 3) Simulate each leg in sequence
    if (!simulateLeg(tri.path[0], ob1)) return -1.0;
    if (!simulateLeg(tri.path[1], ob2)) return -1.0;
    if (!simulateLeg(tri.path[2], ob3)) return -1.0;

    // 4) Final measure of USDT after the 3-leg simulation
    double finalValUSDT = fakeUSDT
                        + (fakeBTC * b3)
                        + (fakeETH * b2);

    double netProfit = finalValUSDT - startValUSDT;
    return netProfit;
}

/**
 * doLeg => either calls doLegLive(...) if liveMode_==true, or does the local depth-based simulation
 */
bool Simulator::doLeg(WalletTransaction& tx,
                      const std::string& pairName,
                      const OrderBookData& ob)
{
    if (liveMode_) {
        // parse side, quantity from local approach, then call doLegLive
        auto [baseAsset, quoteAsset] = parseSymbol(pairName);
        if (quoteAsset == "UNKNOWN") {
            std::cout << "[SIM-LIVE] unknown quote for " << pairName << "\n";
            return false;
        }
        bool isSell = (quoteAsset == "USDT" || quoteAsset == "BTC" || quoteAsset == "BUSD" || quoteAsset == "ETH");
        double freeAmt = (isSell
                          ? wallet_->getFreeBalance(baseAsset)
                          : wallet_->getFreeBalance(quoteAsset));
        double desiredQtyBase = 0.0;
        if (isSell) {
            desiredQtyBase = std::min(freeAmt, volumeLimit_);
            if (desiredQtyBase <= 0.0) {
                std::cout << "[SIM-LIVE] Not enough " << baseAsset << " to sell.\n";
                return false;
            }
        } else {
            double bestAsk = (ob.asks.empty() ? 99999999.0 : ob.asks[0].price);
            double maxSpend= volumeLimit_ * bestAsk;
            double spend   = std::min(freeAmt, maxSpend);
            desiredQtyBase = (bestAsk > 0.0 ? spend / bestAsk : 0.0);
            if (desiredQtyBase <= 0.0) {
                std::cout << "[SIM-LIVE] Not enough " << quoteAsset
                          << " to buy " << baseAsset << "\n";
                return false;
            }
        }
        return doLegLive(tx, pairName, desiredQtyBase, isSell);
    }

    // Otherwise, do the local depth simulation for that leg
    auto t0 = std::chrono::high_resolution_clock::now();
    auto [baseAsset, quoteAsset] = parseSymbol(pairName);
    if (quoteAsset == "UNKNOWN") {
        std::cout << "[SIM] parseSymbol => unknown quote for " << pairName << "\n";
        return false;
    }
    bool isSell = (quoteAsset=="USDT" || quoteAsset=="BTC" || quoteAsset=="BUSD" || quoteAsset=="ETH");
    std::string sideStr = (isSell ? "SELL" : "BUY");

    double freeAmt = (isSell
                      ? wallet_->getFreeBalance(baseAsset)
                      : wallet_->getFreeBalance(quoteAsset));
    double desiredQtyBase=0.0;
    if (isSell) {
        desiredQtyBase = std::min(freeAmt, volumeLimit_);
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] Not enough " << baseAsset << " to sell.\n";
            return false;
        }
    } else {
        double bestAsk = (ob.asks.empty()? 99999999.0 : ob.asks[0].price);
        double maxSpend= volumeLimit_ * bestAsk;
        double spend   = std::min(freeAmt, maxSpend);
        desiredQtyBase = (bestAsk > 0.0? spend / bestAsk : 0.0);
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] Not enough " << quoteAsset << " to buy " << baseAsset << "\n";
            return false;
        }
    }

    // ***CHECK FILTERS***
    // For local sim, approximate price = top of book
    double approximatePrice = 0.0;
    if (isSell) {
        if (!ob.bids.empty()) approximatePrice = ob.bids[0].price;
    } else {
        if (!ob.asks.empty()) approximatePrice = ob.asks[0].price;
    }
    if (approximatePrice <= 0.0) {
        std::cout << "[SIM] no valid price in book to check filters.\n";
        return false;
    }

    if (!passesExchangeFilters(pairName, desiredQtyBase, approximatePrice)) {
        return false;
    }

    double filledQty=0.0, cost=0.0;
    const auto& levels = (isSell ? ob.bids : ob.asks);
    if (levels.empty()) {
        std::cout << "[SIM] no orderbook levels for " << pairName << "\n";
        return false;
    }
    double remaining = desiredQtyBase;
    for (auto& lvl : levels) {
        double tradeQty  = std::min(remaining, lvl.quantity);
        double tradeCost = tradeQty * lvl.price;
        filledQty += tradeQty;
        cost      += tradeCost;
        remaining -= tradeQty;
        if (remaining <= 1e-12) break;
    }
    if (filledQty <= 1e-12) {
        std::cout << "[SIM] no fill at all from depth for " << pairName << "\n";
        return false;
    }
    double avgPrice  = cost / filledQty;
    double fillRatio = filledQty / desiredQtyBase;
    if (fillRatio < minFillRatio_) {
        std::cout << "[SIM] fillRatio=" << fillRatio
                  << " < " << minFillRatio_ << "\n";
        return false;
    }
    double bestPx = (isSell
                     ? (ob.bids.empty()? 0.0 : ob.bids[0].price)
                     : (ob.asks.empty()? 99999999.0 : ob.asks[0].price));
    if (bestPx <= 0.0) {
        std::cout<<"[SIM] bestPx=0 => empty book\n";
        return false;
    }
    double slip = std::fabs(avgPrice - bestPx) / bestPx;
    if (slip > slippageTolerance_) {
        std::cout<<"[SIM] slippage="<< slip <<" > tol="<< slippageTolerance_ <<"\n";
        return false;
    }

    // Apply fee
    double netCostOrProceeds = (isSell
                                ? cost * (1.0 - feePercent_)
                                : cost * (1.0 + feePercent_));

    bool ok1=false, ok2=false;
    if (isSell) {
        ok1 = wallet_->applyChange(tx, baseAsset, -filledQty, 0.0);
        ok2 = wallet_->applyChange(tx, quoteAsset, netCostOrProceeds, 0.0);
    } else {
        ok1 = wallet_->applyChange(tx, quoteAsset, -netCostOrProceeds, 0.0);
        ok2 = wallet_->applyChange(tx, baseAsset, filledQty, 0.0);
    }
    if (!ok1 || !ok2) {
        std::cout << "[SIM] wallet applyChange fail\n";
        return false;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    std::cout << "[SIM] " << sideStr << " on " << pairName
              << " desiredQty=" << desiredQtyBase
              << " filledQty=" << filledQty
              << " avgPrice=" << avgPrice
              << " costOrProceeds=" << cost
              << " time=" << ms << " ms\n";

    logLeg(pairName, sideStr, desiredQtyBase, filledQty,
           fillRatio, slip, ms);
    return true;
}

/**
 * doLegLive => place a real MARKET order via the executor, then update wallet with actual fill result.
 */
bool Simulator::doLegLive(WalletTransaction& tx,
                          const std::string& pairName,
                          double desiredQtyBase,
                          bool isSell)
{
    auto t0 = std::chrono::high_resolution_clock::now();
    std::string sideStr = (isSell ? "SELL" : "BUY");

    // ***CHECK FILTERS*** for live
    // We'll guess a price from the top-of-book or just skip if empty.
    // (In a real bot, you might fetch fresh orderbook or trust the last known.)
    double approximatePrice = 1.0; // fallback
    {
        // We have the "pairName -> orderbook" if needed, but in doLegLive we only get a single "desiredQtyBase"
        // This is a simplified approach: let's guess we can fetch the top-of-book from your aggregator or pass it in.
        // For now, let's assume a fixed "approximate" or user can store a global map of last OB price.
        // We'll do a minimal approach to illustrate the filter.
        if (symbolFilters_.count(pairName)) {
            // e.g. fallback to a rough guess, or store a "last known best price" in a global if you prefer
            approximatePrice = 30000.0; // example fallback
        }
    }

    if (!passesExchangeFilters(pairName, desiredQtyBase, approximatePrice)) {
        std::cout << "[SIM-LIVE] " << pairName << " fails minQty or minNotional filter.\n";
        return false;
    }

    // 1) place real market order
    OrderSide sideEnum = (isSell ? OrderSide::SELL : OrderSide::BUY);
    OrderResult orderRes = executor_->placeMarketOrder(pairName, sideEnum, desiredQtyBase);
    if(!orderRes.success || orderRes.filledQuantity <= 0.0) {
        std::cout << "[SIM-LIVE] placeMarketOrder fail or no fill. msg=" << orderRes.message << "\n";
        return false;
    }
    double fillQtyBase = orderRes.filledQuantity;
    double fillRatio   = fillQtyBase / desiredQtyBase;
    if (fillRatio < minFillRatio_) {
        std::cout << "[SIM-LIVE] fill ratio < " << minFillRatio_
                  << " => " << fillRatio << "\n";
        return false;
    }

    // Fees: if SELL, proceeds go down; if BUY, cost goes up
    double netCostOrProceeds = orderRes.costOrProceeds;
    if (isSell) {
        netCostOrProceeds *= (1.0 - feePercent_);
    } else {
        netCostOrProceeds *= (1.0 + feePercent_);
    }

    auto [baseAsset, quoteAsset] = parseSymbol(pairName);
    bool ok1=false, ok2=false;
    if (isSell) {
        // We sold base => remove base qty, add quote
        ok1 = wallet_->applyChange(tx, baseAsset, -fillQtyBase, 0.0);
        ok2 = wallet_->applyChange(tx, quoteAsset, netCostOrProceeds, 0.0);
    } else {
        // We bought base => remove quote cost, add base
        ok1 = wallet_->applyChange(tx, quoteAsset, -netCostOrProceeds, 0.0);
        ok2 = wallet_->applyChange(tx, baseAsset, fillQtyBase, 0.0);
    }
    if (!ok1 || !ok2) {
        std::cout << "[SIM-LIVE] wallet applyChange fail\n";
        return false;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // Log
    std::cout << "[SIM-LIVE] " << sideStr << " " << fillQtyBase
              << " base on " << pairName
              << " avgPrice=" << orderRes.avgPrice
              << " costOrProceeds=" << orderRes.costOrProceeds
              << " time=" << ms << " ms\n";

    logLeg(pairName, sideStr, desiredQtyBase, fillQtyBase,
           fillRatio, /*slip=*/0.0, ms);
    return true;
}

/**
 * Optional stub for older approach if needed.
 */
double Simulator::simulateTrade(const Triangle& /*tri*/,
                                double /*currentBalance*/,
                                double /*bid1*/, double /*ask1*/,
                                double /*bid2*/, double /*ask2*/,
                                double /*bid3*/, double /*ask3*/)
{
    // original stub
    return 0.0;
}

void Simulator::printWallet() const {
    wallet_->printAll();
}

// Logs the entire 3-leg trade
void Simulator::logTrade(const std::string& path,
                         double startVal,
                         double endVal,
                         double profitPercent)
{
    std::ofstream file(logFileName_, std::ios::app);
    if (!file.is_open()) return;

    auto now  = std::chrono::system_clock::now();
    auto now_c= std::chrono::system_clock::to_time_t(now);

    file << std::put_time(std::localtime(&now_c), "%F %T") << ","
         << path       << ","
         << startVal   << ","
         << endVal     << ","
         << profitPercent << "\n";
}

// Logs each leg's detail
void Simulator::logLeg(const std::string& pairName,
                       const std::string& side,
                       double requestedQty,
                       double filledQty,
                       double fillRatio,
                       double slipPct,
                       double latencyMs)
{
    static const char* LEG_LOG_FILE = "leg_log.csv";
    static bool headerWritten = false;
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lg(logMutex);

    std::ofstream f(LEG_LOG_FILE, std::ios::app);
    if(!f.is_open()) return;

    if(!headerWritten) {
        f << "timestamp,pair,side,requestedQty,filledQty,fillRatio,slippage,latencyMs\n";
        headerWritten = true;
    }
    auto now  = std::chrono::system_clock::now();
    auto now_c= std::chrono::system_clock::to_time_t(now);

    f << std::put_time(std::localtime(&now_c), "%F %T") << ","
      << pairName   << ","
      << side       << ","
      << requestedQty << ","
      << filledQty  << ","
      << fillRatio  << ","
      << slipPct    << ","
      << latencyMs  << "\n";
}

int Simulator::getTotalTrades() const {
    return totalTrades_;
}

double Simulator::getCumulativeProfit() const {
    return cumulativeProfit_;
}

std::vector<std::string> Simulator::getAssetsForPair(const std::string& pairName) const {
    auto [b, q] = parseSymbol(pairName);
    if (q == "UNKNOWN") {
        return {};
    }
    return { b, q };
}
