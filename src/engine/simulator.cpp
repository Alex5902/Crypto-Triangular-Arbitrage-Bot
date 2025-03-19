#include "engine/simulator.hpp"
#include "engine/triangle_scanner.hpp"
#include "core/orderbook.hpp"
#include "core/wallet.hpp"
#include "exchange/i_exchange_executor.hpp"
#include <iostream>
#include <sstream>
#include <fstream>
#include <iomanip>
#include <chrono>
#include <ctime>

#include <cmath> // for min, etc.

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
    // optionally start fresh log
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
    // approximate old value in USDT
    double b1 = (ob1.bids.empty()?0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty()?0.0 : ob2.bids[0].price);
    // If tri has 3rd path => ob3...
    double b3 = (ob3.bids.empty()?0.0 : ob3.bids[0].price);

    double oldValUSDT = 
        wallet_->getFreeBalance("BTC") * b1 +
        wallet_->getFreeBalance("ETH") * b2 +
        wallet_->getFreeBalance("USDT");

    // start transaction
    auto tx = wallet_->beginTransaction();

    // leg1
    double topPrice1 = (ob1.bids.empty()?0.0 : ob1.bids[0].price);
    if (!doLeg(tx, tri.path[0], topPrice1)) {
        std::cout << "[SIM] Leg1 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    // leg2
    double topPrice2 = (ob2.bids.empty()?0.0 : ob2.bids[0].price);
    if (!doLeg(tx, tri.path[1], topPrice2)) {
        std::cout << "[SIM] Leg2 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    // leg3
    double topPrice3 = (ob3.bids.empty()?0.0 : ob3.bids[0].price);
    if (!doLeg(tx, tri.path[2], topPrice3)) {
        std::cout << "[SIM] Leg3 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }

    // commit
    wallet_->commitTransaction(tx);

    // measure newVal
    double nb1 = (ob3.bids.empty()?0.0 : ob3.bids[0].price);
    double nb2 = (ob2.bids.empty()?0.0 : ob2.bids[0].price);
    double newVal = 
        wallet_->getFreeBalance("BTC") * nb1 +
        wallet_->getFreeBalance("ETH") * nb2 +
        wallet_->getFreeBalance("USDT");

    double profitPercent = 0.0;
    if (oldValUSDT > 0.0) {
        profitPercent = ((newVal - oldValUSDT)/ oldValUSDT)*100.0;
    }

    // build path
    std::stringstream ps;
    for (size_t i=0; i< tri.path.size(); i++) {
        if (i>0) ps << "->";
        ps << tri.path[i];
    }
    logTrade(ps.str(), oldValUSDT, newVal, profitPercent);

    std::cout << "[SIM] Traded triangle: " << ps.str()
              << " oldVal=" << oldValUSDT
              << " newVal=" << newVal
              << " profit=" << profitPercent << "%\n";
    return true;
}

bool Simulator::doLeg(WalletTransaction& tx,
                      const std::string& pairName,
                      double topOfBookPrice)
{
    if (!executor_) {
        std::cerr << "[SIM] No exchange executor set!\n";
        return false;
    }
    // decide side
    // if "BTCUSDT" => SELL BTC
    // if "ETHUSDT" => BUY ETH
    // etc. We'll do 2 examples:

    if (pairName == "BTCUSDT") {
        double freeBtc = wallet_->getFreeBalance("BTC");
        double canSell = std::min(freeBtc, volumeLimit_);
        if (canSell<=0.0) {
            std::cout << "[SIM] No BTC to sell.\n";
            return false;
        }

        // place market SELL
        auto t0 = std::chrono::high_resolution_clock::now();
        OrderResult res = executor_->placeMarketOrder("BTCUSDT", OrderSide::SELL, canSell);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double,std::milli>(t1 - t0).count();

        if (!res.success || res.filledQuantity<=0.0) {
            std::cout << "[SIM] SELL fail or no fill\n";
            return false;
        }
        // check fill ratio
        double fillRatio = res.filledQuantity / canSell;
        if (fillRatio < minFillRatio_) {
            std::cout << "[SIM] fill ratio < minFill. Aborting.\n";
            return false;
        }
        // slippage check if you want => compare res.avgPrice with topOfBookPrice
        double slip = std::fabs(res.avgPrice - topOfBookPrice)/ topOfBookPrice;
        if (slip > slippageTolerance_) {
            std::cout << "[SIM] slippage too high. " << slip << "\n";
            return false;
        }

        // applyChange => reduce BTC, add USDT
        double netProceeds = res.costOrProceeds*(1.0 - feePercent_);

        bool ok1 = wallet_->applyChange(tx, "BTC", -(res.filledQuantity), 0.0);
        if (!ok1) return false;
        bool ok2 = wallet_->applyChange(tx, "USDT", netProceeds, 0.0);
        if (!ok2) return false;

        std::cout << "[SIM] SELL " << res.filledQuantity << " BTC => " << netProceeds
                  << " USDT in " << ms << " ms\n";
        return true;
    }
    else if (pairName == "ETHUSDT") {
        double freeUsdt = wallet_->getFreeBalance("USDT");
        if (freeUsdt<=0.0) {
            std::cout << "[SIM] No USDT.\n";
            return false;
        }
        double costMax = volumeLimit_ * topOfBookPrice;
        double spend = std::min(freeUsdt, costMax);
        if (spend<=0.0) return false;

        // place market BUY
        // quantity in base => spend / price
        double baseQty = spend / topOfBookPrice;
        auto t0 = std::chrono::high_resolution_clock::now();
        OrderResult res = executor_->placeMarketOrder("ETHUSDT", OrderSide::BUY, baseQty);
        auto t1 = std::chrono::high_resolution_clock::now();
        double ms = std::chrono::duration<double,std::milli>(t1 - t0).count();

        if (!res.success || res.filledQuantity<=0.0) {
            std::cout << "[SIM] BUY fail or no fill\n";
            return false;
        }
        double fillRatio = res.filledQuantity / baseQty;
        if (fillRatio < minFillRatio_) {
            std::cout << "[SIM] fill ratio < min.\n";
            return false;
        }
        double slip = std::fabs(res.avgPrice - topOfBookPrice)/topOfBookPrice;
        if (slip> slippageTolerance_) {
            std::cout << "[SIM] slippage too high.\n";
            return false;
        }

        // final cost = res.filledQuantity * res.avgPrice
        double costUsed = res.costOrProceeds; 
        double netCost = costUsed*(1.0 + feePercent_); // if you want to interpret fees differently

        bool ok1 = wallet_->applyChange(tx, "USDT", -netCost, 0.0);
        if (!ok1) return false;
        bool ok2 = wallet_->applyChange(tx, "ETH", res.filledQuantity, 0.0);
        if (!ok2) return false;

        std::cout << "[SIM] BUY " << res.filledQuantity << " ETH cost=" << netCost
                  << " in " << ms << " ms\n";
        return true;
    }
    // else e.g. "ETHBTC" -> do the logic. If path is reversed, do your approach

    std::cout << "[SIM] unknown pair: " << pairName << "\n";
    return false;
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
