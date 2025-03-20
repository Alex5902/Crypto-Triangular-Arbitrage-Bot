#include "engine/simulator.hpp"
#include "exchange/binance_dry_executor.hpp"
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
  , executor_(executor)
{
    // Initialize locks if empty
    if (assetLocks_.empty()) {
        assetLocks_.try_emplace("BTC");
        assetLocks_.try_emplace("ETH");
        assetLocks_.try_emplace("USDT");
    }

    // Optionally start fresh log (for the summary of 3-leg trades)
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
    // 1) figure out oldVal in USDT (roughly)
    double b1 = (ob1.bids.empty() ? 0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty() ? 0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty() ? 0.0 : ob3.bids[0].price);

    double oldValUSDT =
        wallet_->getFreeBalance("BTC") * b1 +
        wallet_->getFreeBalance("ETH") * b2 +
        wallet_->getFreeBalance("USDT");

    // 2) Lock all assets used by tri.path for the entire 3-leg trade
    std::vector<std::string> allAssets;
    for (auto& p : tri.path) {
        auto pairAssets = getAssetsForPair(p);
        for (auto& a : pairAssets) {
            if (std::find(allAssets.begin(), allAssets.end(), a) == allAssets.end()) {
                allAssets.push_back(a);
            }
        }
    }
    // sort & lock them to avoid deadlock
    std::sort(allAssets.begin(), allAssets.end());
    std::vector<std::unique_lock<std::mutex>> lockGuards;
    lockGuards.reserve(allAssets.size());
    for (auto& asset : allAssets) {
        lockGuards.emplace_back(assetLocks_[asset]);
    }

    // 3) start transaction
    auto tx = wallet_->beginTransaction();

    // Attempt leg1
    double topPrice1 = b1;
    if (!doLeg(tx, tri.path[0], topPrice1)) {
        std::cout << "[SIM] Leg1 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }

    // Attempt leg2
    double topPrice2 = b2;
    if (!doLeg(tx, tri.path[1], topPrice2)) {
        std::cout << "[SIM] Leg2 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }

    // Attempt leg3
    double topPrice3 = b3;
    if (!doLeg(tx, tri.path[2], topPrice3)) {
        std::cout << "[SIM] Leg3 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }

    // 4) commit (all legs succeeded)
    wallet_->commitTransaction(tx);

    // measure newVal
    double newValUSDT =
        wallet_->getFreeBalance("BTC") * b3 +
        wallet_->getFreeBalance("ETH") * b2 +
        wallet_->getFreeBalance("USDT");

    double profitPercent = 0.0;
    double absoluteProfit = (newValUSDT - oldValUSDT);
    if (oldValUSDT > 0.0) {
        profitPercent = (absoluteProfit / oldValUSDT) * 100.0;
    }

    // build path for logging
    std::stringstream ps;
    for (size_t i = 0; i < tri.path.size(); i++) {
        if (i > 0) ps << "->";
        ps << tri.path[i];
    }
    logTrade(ps.str(), oldValUSDT, newValUSDT, profitPercent);

    // update counters for TUI
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

// doLeg => place an order, check fill ratio, slippage, apply wallet changes
// now with simple RETRY logic (up to 3 attempts)
bool Simulator::doLeg(WalletTransaction& tx,
                      const std::string& pairName,
                      double topOfBookPrice)
{
    if (!executor_) {
        std::cerr << "[SIM] No exchange executor set!\n";
        return false;
    }

    // Identify base & quote, plus side
    std::string baseAsset, quoteAsset, sideStr;
    double freeAmt = 0.0;

    if (pairName == "BTCUSDT") {
        baseAsset  = "BTC";
        quoteAsset = "USDT";
        sideStr    = "SELL";
        freeAmt    = wallet_->getFreeBalance(baseAsset);
    }
    else if (pairName == "ETHUSDT") {
        baseAsset  = "ETH";
        quoteAsset = "USDT";
        sideStr    = "BUY";
        freeAmt    = wallet_->getFreeBalance(quoteAsset);
    }
    else if (pairName == "ETHBTC") {
        baseAsset  = "ETH";
        quoteAsset = "BTC";
        sideStr    = "SELL";
        freeAmt    = wallet_->getFreeBalance(baseAsset);
    }
    else {
        std::cout << "[SIM] Unknown pair " << pairName << "\n";
        return false;
    }

    // Decide how much we want to do
    double desiredQtyBase = 0.0;
    if (sideStr == "SELL") {
        desiredQtyBase = std::min(freeAmt, volumeLimit_);
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] No " << baseAsset << " to sell.\n";
            return false;
        }
    } else {
        double maxSpend = volumeLimit_ * topOfBookPrice;
        double spend = std::min(freeAmt, maxSpend);
        desiredQtyBase = spend / topOfBookPrice;
        if (desiredQtyBase <= 0.0) {
            std::cout << "[SIM] Not enough " << quoteAsset << ".\n";
            return false;
        }
    }

    // If it's a dry executor, we sync mock price
    if (auto* dryExec = dynamic_cast<BinanceDryExecutor*>(executor_)) {
        dryExec->setMockPrice(topOfBookPrice);
    }

    // Retry logic
    const int MAX_RETRIES = 3;
    for (int attempt = 1; attempt <= MAX_RETRIES; ++attempt)
    {
        auto t0 = std::chrono::high_resolution_clock::now();

        OrderSide sideEnum = (sideStr == "SELL") ? OrderSide::SELL : OrderSide::BUY;
        OrderResult res = executor_->placeMarketOrder(pairName, sideEnum, desiredQtyBase);

        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double, std::milli>(t1 - t0).count();

        // 1) Did the order fail entirely?
        if (!res.success || res.filledQuantity <= 0.0) {
            std::cout << "[SIM] Attempt=" << attempt << " " << sideStr
                      << " fail or no fill. filledQty=" << res.filledQuantity << "\n";

            if (attempt < MAX_RETRIES) {
                std::cout << "[SIM] Retrying in 500ms...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // no more retries
            return false;
        }

        // 2) Check fill ratio
        double fillRatio = res.filledQuantity / desiredQtyBase;
        if (fillRatio < minFillRatio_) {
            std::cout << "[SIM] Attempt=" << attempt
                      << " fill ratio < " << minFillRatio_
                      << " => " << fillRatio << "\n";

            if (attempt < MAX_RETRIES) {
                std::cout << "[SIM] Retrying in 500ms...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // no more retries
            return false;
        }

        // 3) Check slippage
        double slip = std::fabs(res.avgPrice - topOfBookPrice) / topOfBookPrice;
        if (slip > slippageTolerance_) {
            std::cout << "[SIM] Attempt=" << attempt
                      << " slippage " << slip
                      << " > tol=" << slippageTolerance_ << "\n";

            if (attempt < MAX_RETRIES) {
                std::cout << "[SIM] Retrying in 500ms...\n";
                std::this_thread::sleep_for(std::chrono::milliseconds(500));
                continue;
            }
            // no more retries
            return false;
        }

        // If we get here => the order is good enough; apply wallet changes
        double netProceedsOrCost = res.costOrProceeds;
        if (sideStr == "SELL") {
            double netProceeds = netProceedsOrCost * (1.0 - feePercent_);
            bool ok1 = wallet_->applyChange(tx, baseAsset, -(res.filledQuantity), 0.0);
            bool ok2 = wallet_->applyChange(tx, quoteAsset, netProceeds, 0.0);
            if (!ok1 || !ok2) {
                std::cout << "[SIM] wallet applyChange fail.\n";
                return false;
            }
        } else {
            double netCost = netProceedsOrCost * (1.0 + feePercent_);
            bool ok1 = wallet_->applyChange(tx, quoteAsset, -netCost, 0.0);
            bool ok2 = wallet_->applyChange(tx, baseAsset, res.filledQuantity, 0.0);
            if (!ok1 || !ok2) {
                std::cout << "[SIM] wallet applyChange fail.\n";
                return false;
            }
        }

        // successful final attempt -> print and log
        std::cout << "[SIM] Attempt=" << attempt
                  << " " << sideStr << " " << res.filledQuantity
                  << " base on " << pairName
                  << " cost/proceeds=" << res.costOrProceeds
                  << " time=" << ms << " ms\n";

        logLeg(pairName, sideStr, desiredQtyBase, res.filledQuantity,
               fillRatio, slip, ms);

        // done
        return true;
    }

    // if we exit the for loop, all attempts failed
    return false;
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

double Simulator::simulateTrade(const Triangle& /*tri*/,
                                double /*currentBalance*/,
                                double /*bid1*/, double /*ask1*/,
                                double /*bid2*/, double /*ask2*/,
                                double /*bid3*/, double /*ask3*/)
{
    // Original stub
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
    static const char* LEG_LOG_FILE = "leg_log.csv";
    static bool headerWritten = false;
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lg(logMutex);

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

// For TUI
int Simulator::getTotalTrades() const {
    return totalTrades_;
}

double Simulator::getCumulativeProfit() const {
    return cumulativeProfit_;
}
