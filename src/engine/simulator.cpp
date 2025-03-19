#include "engine/simulator.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>
#include <cmath>

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
        if (assetLocks_.empty()) {
            assetLocks_.try_emplace("BTC");
            assetLocks_.try_emplace("ETH");
            assetLocks_.try_emplace("USDT");
        }        
    }
    
    // optionally start fresh log
    std::ofstream file(logFileName_, std::ios::app);
    if (file.is_open()) {
        file << "timestamp,path,start_val,end_val,profit_percent\n";
    }
}

// This is the main multi-leg function
bool Simulator::simulateTradeDepthWithWallet(const Triangle& tri,
                                             const OrderBookData& ob1,
                                             const OrderBookData& ob2,
                                             const OrderBookData& ob3)
{
    // 1) figure out oldVal in USDT
    double b1 = (ob1.bids.empty()?0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty()?0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty()?0.0 : ob3.bids[0].price);

    double oldValUSDT =
        wallet_->getFreeBalance("BTC") * b1 +
        wallet_->getFreeBalance("ETH") * b2 +
        wallet_->getFreeBalance("USDT");

    // 2) Lock all assets used by tri.path for the entire 3-leg trade
    // e.g. if tri.path = [BTCUSDT, ETHUSDT, ETHBTC], we'll lock {BTC,USDT,ETH}
    // We'll do a small vector of locks
    std::vector<std::string> allAssets;
    for (auto& p : tri.path) {
        auto pairAssets = getAssetsForPair(p);
        for (auto& a : pairAssets) {
            if (std::find(allAssets.begin(), allAssets.end(), a) == allAssets.end()) {
                allAssets.push_back(a);
            }
        }
    }
    // Now lock them in alphabetical order to avoid deadlock
    std::sort(allAssets.begin(), allAssets.end());
    std::vector<std::unique_lock<std::mutex>> lockGuards;
    lockGuards.reserve(allAssets.size());
    for (auto& asset : allAssets) {
        lockGuards.emplace_back(assetLocks_[asset]);
    }

    // 3) start transaction
    auto tx = wallet_->beginTransaction();

    // leg1
    double topPrice1 = b1; // or ob1.bids[0].price
    if (!doLeg(tx, tri.path[0], topPrice1)) {
        std::cout << "[SIM] Leg1 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    // leg2
    double topPrice2 = b2;
    if (!doLeg(tx, tri.path[1], topPrice2)) {
        std::cout << "[SIM] Leg2 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    // leg3
    double topPrice3 = b3;
    if (!doLeg(tx, tri.path[2], topPrice3)) {
        std::cout << "[SIM] Leg3 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }

    // 4) commit
    wallet_->commitTransaction(tx);

    // measure newVal
    double newValUSDT =
        wallet_->getFreeBalance("BTC") * b3 +
        wallet_->getFreeBalance("ETH") * b2 +
        wallet_->getFreeBalance("USDT");

    double profitPercent = 0.0;
    if (oldValUSDT > 0.0) {
        profitPercent = ((newValUSDT - oldValUSDT)/ oldValUSDT)*100.0;
    }

    // build path
    std::stringstream ps;
    for (size_t i=0; i < tri.path.size(); i++) {
        if (i>0) ps << "->";
        ps << tri.path[i];
    }
    logTrade(ps.str(), oldValUSDT, newValUSDT, profitPercent);

    std::cout << "[SIM] Traded triangle: " << ps.str()
              << " oldVal=" << oldValUSDT
              << " newVal=" << newValUSDT
              << " profit=" << profitPercent << "%\n";
    return true;
}

// doLeg => place an order, check fill ratio, slippage, apply wallet changes
bool Simulator::doLeg(WalletTransaction& tx,
                      const std::string& pairName,
                      double topOfBookPrice)
{
    if (!executor_) {
        std::cerr << "[SIM] No exchange executor set!\n";
        return false;
    }

    // Decide side
    // e.g. if "BTCUSDT", we SELL BTC
    // if "ETHUSDT", we BUY ETH
    // We'll do 2 examples:

    std::string sideStr;
    double freeAmt=0.0; // how much we can attempt
    if (pairName == "BTCUSDT") {
        sideStr = "SELL";
        freeAmt = wallet_->getFreeBalance("BTC");
    }
    else if (pairName == "ETHUSDT") {
        sideStr = "BUY";
        freeAmt = wallet_->getFreeBalance("USDT");
    }
    else {
        std::cout << "[SIM] Unknown pair " << pairName << "\n";
        return false;
    }

    // figure how much we want to do
    double desiredQtyBase = 0.0; // quantity in base if SELL, or if BUY
    if (sideStr == "SELL") {
        desiredQtyBase = std::min(freeAmt, volumeLimit_);
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] No " << "BTC" << " to sell.\n";
            return false;
        }
    } else {
        // side=BUY => we have free USDT in freeAmt
        // how many base can we buy if we spend min(freeAmt, volumeLimit_ * topOfBookPrice)?
        double maxSpend = volumeLimit_ * topOfBookPrice; 
        double spend = std::min(freeAmt, maxSpend);
        desiredQtyBase = spend / topOfBookPrice;
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] Not enough USDT.\n";
            return false;
        }
    }

    // measure latency
    auto t0 = std::chrono::high_resolution_clock::now();
    OrderSide sideEnum = (sideStr=="SELL") ? OrderSide::SELL : OrderSide::BUY;
    OrderResult res = executor_->placeMarketOrder(pairName, sideEnum, desiredQtyBase);
    auto t1 = std::chrono::high_resolution_clock::now();
    double ms = std::chrono::duration<double,std::milli>(t1 - t0).count();

    if (!res.success || res.filledQuantity <= 0.0) {
        std::cout << "[SIM] " << sideStr << " fail or no fill\n";
        return false;
    }

    double fillRatio = res.filledQuantity / desiredQtyBase;
    if (fillRatio < minFillRatio_) {
        std::cout << "[SIM] fill ratio < " << minFillRatio_ << "\n";
        return false;
    }
    double slip = std::fabs(res.avgPrice - topOfBookPrice)/topOfBookPrice;
    if (slip > slippageTolerance_) {
        std::cout << "[SIM] slippage " << slip << " > tol\n";
        return false;
    }

    // apply fees
    // if SELL => net proceeds
    // if BUY => cost is res.filledQuantity * res.avgPrice
    if (sideStr=="SELL") {
        double netProceeds = res.costOrProceeds*(1.0 - feePercent_);
        // remove BTC, add USDT
        bool ok1 = wallet_->applyChange(tx, "BTC", -(res.filledQuantity), 0.0);
        bool ok2 = wallet_->applyChange(tx, "USDT", netProceeds, 0.0);
        if(!ok1 || !ok2) return false;
    } else {
        // BUY => we spent costOrProceeds
        double netCost = res.costOrProceeds*(1.0 + feePercent_);
        bool ok1 = wallet_->applyChange(tx, "USDT", -netCost, 0.0);
        bool ok2 = wallet_->applyChange(tx, "ETH", res.filledQuantity, 0.0);
        if(!ok1 || !ok2) return false;
    }

    // Print a quick summary
    std::cout << "[SIM] " << sideStr << " " << res.filledQuantity
              << " base on " << pairName
              << " cost/proceeds=" << res.costOrProceeds
              << " time=" << ms << " ms\n";

    // log the leg
    logLeg(pairName, sideStr, desiredQtyBase, res.filledQuantity,
           fillRatio, slip, ms);
    return true;
}

// We use a simple approach: "BTCUSDT" => assets {BTC,USDT}, "ETHUSDT" => {ETH,USDT}, etc.
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
    // add more if you have "BNBUSDT", etc.
    return assets;
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

// logs the entire 3-leg trade
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

// logs each leg's detail
void Simulator::logLeg(const std::string& pairName,
                       const std::string& side,
                       double requestedQty,
                       double filledQty,
                       double fillRatio,
                       double slipPct,
                       double latencyMs)
{
    // We'll write to an extra CSV file for leg-level detail
    static const char* LEG_LOG_FILE = "leg_log.csv";
    static bool headerWritten = false;

    std::ofstream f(LEG_LOG_FILE, std::ios::app);
    if (f.is_open()) {
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
}
