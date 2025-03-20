#include "engine/simulator.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cmath>
#include <thread>

std::map<std::string, std::mutex> Simulator::assetLocks_;

Simulator::Simulator(const std::string& logFileName,
                     double feePercent,
                     double slippageTolerance,
                     double volumeLimit,
                     double minFillRatio,
                     Wallet* sharedWallet,
                     IExchangeExecutor* executor)
  : logFileName_(logFileName)
  , feePercent_(feePercent)
  , slippageTolerance_(slippageTolerance)
  , volumeLimit_(volumeLimit)
  , minFillRatio_(minFillRatio)
  , wallet_(sharedWallet)
  , executor_(executor) // we won't use it for depth simulation, but it's here if you expand
{
    if (assetLocks_.empty()) {
        assetLocks_.try_emplace("BTC");
        assetLocks_.try_emplace("ETH");
        assetLocks_.try_emplace("USDT");
    }

    // Start or append sim log
    std::ofstream file(logFileName_, std::ios::app);
    if (file.is_open()) {
        file << "timestamp,path,start_val,end_val,profit_percent\n";
    }
}

bool Simulator::simulateTradeDepthWithWallet(const Triangle& tri,
                                             const OrderBookData& ob1,
                                             const OrderBookData& ob2,
                                             const OrderBookData& ob3)
{
    // 1) oldVal
    double b1 = (ob1.bids.empty() ? 0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty() ? 0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty() ? 0.0 : ob3.bids[0].price);

    double oldValUSDT =
        wallet_->getFreeBalance("BTC") * b1 +
        wallet_->getFreeBalance("ETH") * b2 +
        wallet_->getFreeBalance("USDT");

    // 2) Lock assets used
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

    auto tx = wallet_->beginTransaction();

    // leg1
    if (!doLeg(tx, tri.path[0], ob1)) {
        std::cout << "[SIM] Leg1 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    // leg2
    if (!doLeg(tx, tri.path[1], ob2)) {
        std::cout << "[SIM] Leg2 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    // leg3
    if (!doLeg(tx, tri.path[2], ob3)) {
        std::cout << "[SIM] Leg3 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }

    wallet_->commitTransaction(tx);

    // newVal
    double newValUSDT =
        wallet_->getFreeBalance("BTC") * b3 +
        wallet_->getFreeBalance("ETH") * b2 +
        wallet_->getFreeBalance("USDT");

    double profitPercent = 0.0;
    double absoluteProfit = (newValUSDT - oldValUSDT);
    if (oldValUSDT > 0.0) {
        profitPercent = (absoluteProfit / oldValUSDT) * 100.0;
    }

    // build path string
    std::stringstream ps;
    for (size_t i = 0; i < tri.path.size(); i++) {
        if (i > 0) ps << "->";
        ps << tri.path[i];
    }
    logTrade(ps.str(), oldValUSDT, newValUSDT, profitPercent);

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
 * The new doLeg:
 *  - Figures out side (SELL or BUY).
 *  - If SELL: fill from ob.bids; if BUY: fill from ob.asks
 *  - Loops over levels until we fill the desired base quantity or run out of depth
 *  - Averages the fill price
 *  - Checks fill ratio & slippage
 *  - Applies changes to wallet
 */
bool Simulator::doLeg(WalletTransaction& tx,
                      const std::string& pairName,
                      const OrderBookData& ob)
{
    // measure start time for latency
    auto t0 = std::chrono::high_resolution_clock::now();

    // side, baseAsset, quoteAsset
    std::string sideStr;
    std::string baseAsset, quoteAsset;
    double freeAmt = 0.0;

    if (pairName == "BTCUSDT") {
        sideStr = "SELL";
        baseAsset  = "BTC";
        quoteAsset = "USDT";
        freeAmt = wallet_->getFreeBalance(baseAsset);
    }
    else if (pairName == "ETHUSDT") {
        sideStr = "BUY";
        baseAsset  = "ETH";
        quoteAsset = "USDT";
        freeAmt = wallet_->getFreeBalance(quoteAsset);
    }
    else if (pairName == "ETHBTC") {
        sideStr = "SELL";
        baseAsset  = "ETH";
        quoteAsset = "BTC";
        freeAmt = wallet_->getFreeBalance(baseAsset);
    }
    else {
        std::cout << "[SIM] Unknown pair " << pairName << "\n";
        return false;
    }

    // how much do we want to trade in "base" units?
    double desiredQtyBase = 0.0;
    if (sideStr == "SELL") {
        desiredQtyBase = std::min(freeAmt, volumeLimit_);
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] Not enough " << baseAsset << " to sell.\n";
            return false;
        }
    } else {
        // side=BUY => we have free quoteAsset
        // how much base can we buy if we spend up to volumeLimit_ * bestAsk?
        // but we have the entire ask depth, so let's do a top-of-book estimate just for max
        double bestAsk = (ob.asks.empty()) ? 99999999.0 : ob.asks[0].price;
        double maxSpend = volumeLimit_ * bestAsk;
        double spend = std::min(freeAmt, maxSpend);
        desiredQtyBase = spend / bestAsk;
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] Not enough " << quoteAsset << ".\n";
            return false;
        }
    }

    // now we fill across the relevant side of the book
    double filledQty = 0.0;
    double cost = 0.0; // if SELL => cost is what we receive; if BUY => cost is what we pay
    bool isSell = (sideStr == "SELL");

    const auto& levels = (isSell ? ob.bids : ob.asks);
    if (levels.empty()) {
        std::cout << "[SIM] no orderbook levels\n";
        return false;
    }

    double remainingBase = desiredQtyBase;
    for (const auto& lvl : levels) {
        double lvlPrice  = lvl.price;
        double lvlQty    = lvl.quantity; // in base units
        if (lvlQty <= 0.0) continue;

        double tradeQty = std::min(remainingBase, lvlQty);
        double tradeCost = tradeQty * lvlPrice;

        filledQty += tradeQty;
        cost += tradeCost;

        remainingBase -= tradeQty;
        if (remainingBase <= 1e-12) {
            break; // fully filled
        }
    }

    if (filledQty <= 1e-12) {
        std::cout << "[SIM] no fill at all from depth\n";
        return false;
    }

    // average fill price
    double avgPrice = cost / filledQty;

    double fillRatio = filledQty / desiredQtyBase;
    if (fillRatio < minFillRatio_) {
        std::cout << "[SIM] fillRatio=" << fillRatio
                  << " < minFill=" << minFillRatio_ << "\n";
        return false;
    }

    // check slippage against the "best price" for slippage calc
    // if SELL => best price is ob.bids[0].price
    // if BUY  => best price is ob.asks[0].price
    double bestPx = 0.0;
    if (isSell) {
        bestPx = (ob.bids.empty()) ? 0.0 : ob.bids[0].price;
    } else {
        bestPx = (ob.asks.empty()) ? 99999999.0 : ob.asks[0].price;
    }
    if (bestPx <= 0.0) {
        std::cout << "[SIM] bestPx=0 => empty book\n";
        return false;
    }
    double slip = std::fabs(avgPrice - bestPx) / bestPx;
    if (slip > slippageTolerance_) {
        std::cout << "[SIM] slippage=" << slip
                  << " > tol=" << slippageTolerance_ << "\n";
        return false;
    }

    // apply fees
    double netCostOrProceeds = cost;
    if (isSell) {
        // we SELL => we receive proceeds
        // fee => subtract from proceeds
        netCostOrProceeds = cost * (1.0 - feePercent_);
    } else {
        // we BUY => we pay cost
        // fee => + cost
        netCostOrProceeds = cost * (1.0 + feePercent_);
    }

    // apply wallet changes
    bool ok1 = false, ok2 = false;
    if (isSell) {
        // remove filledQty of baseAsset
        ok1 = wallet_->applyChange(tx, baseAsset, -filledQty, 0.0);
        // add net proceeds in quote
        ok2 = wallet_->applyChange(tx, quoteAsset, netCostOrProceeds, 0.0);
    } else {
        // we spent netCostOrProceeds from the quote
        double spent = netCostOrProceeds;
        ok1 = wallet_->applyChange(tx, quoteAsset, -spent, 0.0);
        // we gained filledQty of base
        ok2 = wallet_->applyChange(tx, baseAsset, filledQty, 0.0);
    }
    if (!ok1 || !ok2) {
        std::cout << "[SIM] wallet applyChange fail\n";
        return false;
    }

    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

    // final print + log
    std::cout << "[SIM] " << sideStr << " depth-based fill on " << pairName
              << " desiredQty=" << desiredQtyBase
              << " filledQty=" << filledQty
              << " avgPrice=" << avgPrice
              << " costOrProceeds=" << cost
              << " time=" << ms << " ms\n";

    logLeg(pairName, sideStr, desiredQtyBase, filledQty,
           fillRatio, slip, ms);
    return true;
}

// older approach
double Simulator::simulateTrade(const Triangle& /*tri*/,
                                double /*currentBalance*/,
                                double /*bid1*/, double /*ask1*/,
                                double /*bid2*/, double /*ask2*/,
                                double /*bid3*/, double /*ask3*/)
{
    // stub
    return 0.0;
}

void Simulator::printWallet() const {
    wallet_->printAll();
}

void Simulator::logTrade(const std::string& path,
                         double startVal,
                         double endVal,
                         double profitPercent)
{
    std::ofstream file(logFileName_, std::ios::app);
    if (!file.is_open()) return;

    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);

    file << std::put_time(std::localtime(&now_c), "%F %T") << ","
         << path << ","
         << startVal << ","
         << endVal << ","
         << profitPercent << "\n";
}

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
    if (!f.is_open()) return;

    if (!headerWritten) {
        f << "timestamp,pair,side,requestedQty,filledQty,fillRatio,slippage,latencyMs\n";
        headerWritten = true;
    }
    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);

    f << std::put_time(std::localtime(&now_c), "%F %T") << ","
      << pairName << ","
      << side << ","
      << requestedQty << ","
      << filledQty << ","
      << fillRatio << ","
      << slipPct << ","
      << latencyMs << "\n";
}

int Simulator::getTotalTrades() const {
    return totalTrades_;
}

double Simulator::getCumulativeProfit() const {
    return cumulativeProfit_;
}

std::vector<std::string> Simulator::getAssetsForPair(const std::string& pairName) const {
    std::vector<std::string> assets;
    if (pairName == "BTCUSDT") {
        assets.push_back("BTC");
        assets.push_back("USDT");
    } else if (pairName == "ETHUSDT") {
        assets.push_back("ETH");
        assets.push_back("USDT");
    } else if (pairName == "ETHBTC") {
        assets.push_back("ETH");
        assets.push_back("BTC");
    }
    return assets;
}
