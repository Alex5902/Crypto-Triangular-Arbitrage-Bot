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

// We'll define parseSymbol here
static std::vector<std::string> knownQuotes = {
    "USDT", "BTC", "ETH", "BNB", "BUSD", "USDC"
};

std::pair<std::string,std::string> parseSymbol(const std::string& pair) {
    for (const auto& q : knownQuotes) {
        if (pair.size() > q.size()) {
            size_t pos = pair.rfind(q);
            if (pos != std::string::npos && (pos + q.size()) == pair.size()) {
                // q is suffix
                std::string base = pair.substr(0, pos);
                return { base, q };
            }
        }
    }
    // fallback
    return { pair, "UNKNOWN" };
}

// static map
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
  , executor_(executor)
{
    if (assetLocks_.empty()) {
        assetLocks_.try_emplace("BTC");
        assetLocks_.try_emplace("ETH");
        assetLocks_.try_emplace("USDT");
        assetLocks_.try_emplace("BNB");
        assetLocks_.try_emplace("BUSD");
        assetLocks_.try_emplace("USDC");
    }

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
    // same as before
    double b1 = (ob1.bids.empty() ? 0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty() ? 0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty() ? 0.0 : ob3.bids[0].price);

    double oldValUSDT = wallet_->getFreeBalance("BTC") * b1
                      + wallet_->getFreeBalance("ETH") * b2
                      + wallet_->getFreeBalance("USDT");

    // lock assets
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

    double newValUSDT = wallet_->getFreeBalance("BTC") * b3
                      + wallet_->getFreeBalance("ETH") * b2
                      + wallet_->getFreeBalance("USDT");
    double profitPercent = 0.0;
    double absoluteProfit = (newValUSDT - oldValUSDT);
    if (oldValUSDT > 0.0) {
        profitPercent = (absoluteProfit / oldValUSDT) * 100.0;
    }

    // log
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


bool Simulator::doLeg(WalletTransaction& tx,
                      const std::string& pairName,
                      const OrderBookData& ob)
{
    auto t0 = std::chrono::high_resolution_clock::now();

    // Dynamically parse base/quote
    auto [baseAsset, quoteAsset] = parseSymbol(pairName);
    if (quoteAsset == "UNKNOWN") {
        std::cout << "[SIM] parseSymbol => unknown quote for pair=" 
                  << pairName << "\n";
        return false;
    }

    // Decide side (BUY or SELL) with a simple default approach:
    // If quote is USDT or BTC => we SELL the base
    // else => we BUY the base using the quote
    bool isSell = false;
    if (quoteAsset == "USDT" || quoteAsset == "BTC") {
        isSell = true;
    }

    std::string sideStr = (isSell ? "SELL" : "BUY");

    double freeAmt = 0.0;
    if (isSell) {
        // we have base asset, want to sell
        freeAmt = wallet_->getFreeBalance(baseAsset);
    } else {
        // we want to buy base asset => we have quote
        freeAmt = wallet_->getFreeBalance(quoteAsset);
    }

    // Figure out how much we want to do
    double desiredQtyBase = 0.0;
    if (isSell) {
        desiredQtyBase = std::min(freeAmt, volumeLimit_);
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] Not enough " << baseAsset << " to sell.\n";
            return false;
        }
    } else {
        // If BUY => we have 'freeAmt' of quote asset
        // We'll do a rough top-of-book to guess how much base we can buy
        double bestAsk = (ob.asks.empty() ? 99999999.0 : ob.asks[0].price);
        double maxSpend = volumeLimit_ * bestAsk;
        double spend = std::min(freeAmt, maxSpend);
        desiredQtyBase = (bestAsk > 0.0 ? spend / bestAsk : 0.0);
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] Not enough " << quoteAsset << " to buy " 
                      << baseAsset << "\n";
            return false;
        }
    }

    // Now fill across the relevant side of the book
    double filledQty = 0.0;
    double cost = 0.0; // if SELL => cost is proceeds, if BUY => cost is how much we pay
    const auto& levels = (isSell ? ob.bids : ob.asks);
    if (levels.empty()) {
        std::cout << "[SIM] no orderbook levels for " << pairName << "\n";
        return false;
    }

    double remainingBase = desiredQtyBase;
    for (const auto& lvl : levels) {
        double lvlPrice = lvl.price;
        double lvlQty   = lvl.quantity; // in base units
        if (lvlQty <= 0.0) continue;

        double tradeQty   = std::min(remainingBase, lvlQty);
        double tradeCost  = tradeQty * lvlPrice;

        filledQty  += tradeQty;
        cost       += tradeCost;

        remainingBase -= tradeQty;
        if (remainingBase <= 1e-12) {
            break; // fully filled
        }
    }

    if (filledQty <= 1e-12) {
        std::cout << "[SIM] no fill at all from depth for " << pairName << "\n";
        return false;
    }

    double avgPrice = cost / filledQty;
    double fillRatio = filledQty / desiredQtyBase;
    if (fillRatio < minFillRatio_) {
        std::cout << "[SIM] fillRatio=" << fillRatio
                  << " < minFill=" << minFillRatio_ << "\n";
        return false;
    }

    // slippage check
    double bestPx = 0.0;
    if (isSell) {
        bestPx = (ob.bids.empty() ? 0.0 : ob.bids[0].price);
    } else {
        bestPx = (ob.asks.empty() ? 99999999.0 : ob.asks[0].price);
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
    double netCostOrProceeds = (isSell ? cost * (1.0 - feePercent_)
                                       : cost * (1.0 + feePercent_));

    // wallet changes
    bool ok1 = false, ok2 = false;
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

    std::cout << "[SIM] " << (isSell?"SELL":"BUY") << " dynamic on " << pairName
              << " desiredQty=" << desiredQtyBase
              << " filledQty=" << filledQty
              << " avgPrice=" << avgPrice
              << " costOrProceeds=" << cost
              << " time=" << ms << " ms\n";

    logLeg(pairName, (isSell?"SELL":"BUY"), desiredQtyBase, filledQty,
           fillRatio, slip, ms);
    return true;
}

double Simulator::simulateTrade(const Triangle& /*tri*/,
                                double /*currentBalance*/,
                                double /*bid1*/, double /*ask1*/,
                                double /*bid2*/, double /*ask2*/,
                                double /*bid3*/, double /*ask3*/)
{
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

/** 
 * For locking assets, we just parse the pair to find (base,quote). 
 * Then we return {base, quote} if known, else empty.
 */
std::vector<std::string> Simulator::getAssetsForPair(const std::string& pairName) const {
    auto [b, q] = parseSymbol(pairName);
    if (q == "UNKNOWN") {
        // fallback, no real lock
        return {};
    }
    return { b, q };
}
