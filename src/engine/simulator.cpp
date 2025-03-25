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

// Example known quotes.  parseSymbol will detect them as suffix.
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
    return {pair,"UNKNOWN"};
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
    if (assetLocks_.empty()) {
        assetLocks_.try_emplace("BTC");
        assetLocks_.try_emplace("ETH");
        assetLocks_.try_emplace("USDT");
        assetLocks_.try_emplace("BNB");
        assetLocks_.try_emplace("BUSD");
        assetLocks_.try_emplace("USDC");
    }

    // start or append the sim_log
    std::ofstream file(logFileName_, std::ios::app);
    if (file.is_open()) {
        file << "timestamp,path,start_val,end_val,profit_percent\n";
    }
}

/**
 * The main 3-leg function:
 * 1) We do a quick "estimateTriangleProfit" with the depth to see if final USDT>start USDT
 * 2) If negative => skip
 * 3) If positive => lock assets, do actual legs (live or sim).
 */
bool Simulator::simulateTradeDepthWithWallet(const Triangle& tri,
                                             const OrderBookData& ob1,
                                             const OrderBookData& ob2,
                                             const OrderBookData& ob3)
{
    // 1) figure out oldVal in USDT
    double b1 = (ob1.bids.empty()? 0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty()? 0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty()? 0.0 : ob3.bids[0].price);

    double oldValUSDT = wallet_->getFreeBalance("BTC") * b1 +
                        wallet_->getFreeBalance("ETH") * b2 +
                        wallet_->getFreeBalance("USDT");

    // 2) estimate final USDT if all 3 legs fill
    double estFinal = estimateTriangleProfit(tri, ob1, ob2, ob3, oldValUSDT);
    if (estFinal < 0.0) {
        std::cout << "[SIM] Pre-check => route not profitable or failed. Skipping.\n";
        return false;
    }
    double estProfit = (estFinal - oldValUSDT);
    if (estProfit < minProfitUSDT_) {
        std::cout << "[SIM] Pre-check => estProfit=" << estProfit
                  << " < min=" << minProfitUSDT_ << " USDT => skipping.\n";
        return false;
    }    

    // If we pass the check => proceed
    // Lock all assets used by tri.path
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

    // 3) begin transaction for the actual legs
    auto tx = wallet_->beginTransaction();

    // do the 3 legs (live or sim)
    if (!doLeg(tx, tri.path[0], ob1)) {
        std::cout << "[SIM] Leg1 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    if (!doLeg(tx, tri.path[1], ob2)) {
        std::cout << "[SIM] Leg2 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }
    if (!doLeg(tx, tri.path[2], ob3)) {
        std::cout << "[SIM] Leg3 fail, rollback\n";
        wallet_->rollbackTransaction(tx);
        return false;
    }

    // commit
    wallet_->commitTransaction(tx);

    // measure newVal
    double newValUSDT = wallet_->getFreeBalance("BTC") * b3
                      + wallet_->getFreeBalance("ETH") * b2
                      + wallet_->getFreeBalance("USDT");
    double profitPercent=0.0;
    double absoluteProfit = (newValUSDT - oldValUSDT);
    if (oldValUSDT > 0.0) {
        profitPercent = (absoluteProfit/oldValUSDT)*100.0;
    }

    // build path for logging
    std::stringstream ps;
    for (size_t i=0; i< tri.path.size(); i++){
        if(i>0) ps<<"->";
        ps<< tri.path[i];
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
 * estimateTriangleProfit => do a "dry-run" of 3 legs using the orderbook,
 * applying fees & slippage, returning final USDT. If anything fails => return <0.
 */
double Simulator::estimateTriangleProfit(const Triangle& tri,
                                         const OrderBookData& ob1,
                                         const OrderBookData& ob2,
                                         const OrderBookData& ob3,
                                         double startValUSDT)
{
    // We'll track an imaginary wallet with { BTC, ETH, USDT } amounts from the real wallet
    // Then each leg is "simulated" in depth, no real changes.
    double b1 = (ob1.bids.empty()?0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty()?0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty()?0.0 : ob3.bids[0].price);

    double fakeBTC = wallet_->getFreeBalance("BTC");
    double fakeETH = wallet_->getFreeBalance("ETH");
    double fakeUSDT= wallet_->getFreeBalance("USDT");

    // define a small lambda to do one leg
    auto simulateLeg = [&](const std::string& pairName, const OrderBookData& ob)->bool {
        auto [baseAsset, quoteAsset] = parseSymbol(pairName);
        if(quoteAsset=="UNKNOWN") {
            return false;
        }
        bool isSell = (quoteAsset=="USDT"||quoteAsset=="BTC");
        double freeAmt=0.0;
        if (isSell) {
            // we have base
            if(baseAsset=="BTC") freeAmt=fakeBTC;
            else if(baseAsset=="ETH") freeAmt=fakeETH;
            else freeAmt=0.0; 
        } else {
            // we have quote
            if(quoteAsset=="BTC") freeAmt=fakeBTC;
            else if(quoteAsset=="USDT") freeAmt=fakeUSDT;
            else if(quoteAsset=="ETH") freeAmt=fakeETH;
            else freeAmt=0.0;
        }
        // same logic as doLeg
        double desiredQtyBase=0.0;
        if(isSell) {
            desiredQtyBase=std::min(freeAmt, volumeLimit_);
            if(desiredQtyBase<=0.0) return false;
        } else {
            double bestAsk=(ob.asks.empty()?99999999.0:ob.asks[0].price);
            double maxSpend=volumeLimit_ * bestAsk;
            double spend=std::min(freeAmt,maxSpend);
            desiredQtyBase=(bestAsk>0.0 ? spend/bestAsk:0.0);
            if(desiredQtyBase<=0.0) return false;
        }
        // fill across relevant side
        double filledQty=0.0, cost=0.0;
        const auto& levels=(isSell? ob.bids: ob.asks);
        double remaining=desiredQtyBase;
        for(auto& lvl: levels){
            double tradeQty= std::min(remaining, lvl.quantity);
            double tradeCost= tradeQty* lvl.price;
            filledQty+=tradeQty;
            cost     +=tradeCost;
            remaining-=tradeQty;
            if(remaining<=1e-12) break;
        }
        if(filledQty<=1e-12) return false;
        double fillRatio= filledQty/ desiredQtyBase;
        if(fillRatio< minFillRatio_) return false;

        // slippage check
        double bestPx=0.0;
        if(isSell) bestPx=(ob.bids.empty()?0.0: ob.bids[0].price);
        else       bestPx=(ob.asks.empty()?99999999.0: ob.asks[0].price);
        if(bestPx<=0.0) return false;
        double avgPx= cost/ filledQty;
        double slip= std::fabs(avgPx- bestPx)/ bestPx;
        if(slip> slippageTolerance_) return false;

        // apply fee 
        double netCostOrProceeds= (isSell? cost*(1.0- feePercent_): cost*(1.0+ feePercent_));

        // update "fake" wallet
        if(isSell) {
            if(baseAsset=="BTC") fakeBTC -= filledQty;
            else if(baseAsset=="ETH") fakeETH -= filledQty;
            // add to quote
            if(quoteAsset=="USDT") fakeUSDT += netCostOrProceeds;
            else if(quoteAsset=="BTC") fakeBTC += netCostOrProceeds;
            else if(quoteAsset=="ETH") fakeETH += netCostOrProceeds;
        } else {
            // buy => spend quote, gain base
            if(quoteAsset=="USDT") fakeUSDT -= netCostOrProceeds;
            else if(quoteAsset=="BTC") fakeBTC -= netCostOrProceeds;
            else if(quoteAsset=="ETH") fakeETH -= netCostOrProceeds;

            if(baseAsset=="BTC") fakeBTC += filledQty;
            else if(baseAsset=="ETH") fakeETH += filledQty;
        }
        // done
        return true;
    };

    // Leg1
    if(!simulateLeg(tri.path[0], ob1)) return -1.0;
    // Leg2
    if(!simulateLeg(tri.path[1], ob2)) return -1.0;
    // Leg3
    if(!simulateLeg(tri.path[2], ob3)) return -1.0;

    // final measure: how much USDT are we worth now?
    // We'll assume last known top-of-book from each ob for "marking" the asset.
    // Or just use b1,b2,b3
    // We'll pick whichever is relevant:
    double finalVal = fakeUSDT
                    + fakeBTC*b3
                    + fakeETH*b2; // or you might do separate logic 
    return finalVal;
}

/** 
 * doLeg => either calls doLegLive if liveMode_==true, or does the normal "depth sim" approach.
 */
bool Simulator::doLeg(WalletTransaction& tx,
                      const std::string& pairName,
                      const OrderBookData& ob)
{
    if (liveMode_) {
        // parse side, quantity from local approach, then call doLegLive
        auto [baseAsset, quoteAsset] = parseSymbol(pairName);
        if (quoteAsset=="UNKNOWN") {
            std::cout<<"[SIM-LIVE] unknown quote for "<<pairName<<"\n";
            return false;
        }
        bool isSell=(quoteAsset=="USDT"||quoteAsset=="BTC");
        double freeAmt= (isSell
                         ? wallet_->getFreeBalance(baseAsset)
                         : wallet_->getFreeBalance(quoteAsset));
        double desiredQtyBase=0.0;
        if(isSell){
            desiredQtyBase= std::min(freeAmt, volumeLimit_);
            if(desiredQtyBase<=0.0){
                std::cout<<"[SIM-LIVE] Not enough "<<baseAsset<<" to sell.\n";
                return false;
            }
        } else {
            double bestAsk=(ob.asks.empty()?99999999.0: ob.asks[0].price);
            double maxSpend= volumeLimit_*bestAsk;
            double spend= std::min(freeAmt, maxSpend);
            desiredQtyBase=(bestAsk>0.0? spend/bestAsk: 0.0);
            if(desiredQtyBase<=0.0){
                std::cout<<"[SIM-LIVE] Not enough "<<quoteAsset<<" to buy "<<baseAsset<<"\n";
                return false;
            }
        }
        return doLegLive(tx,pairName,desiredQtyBase,isSell);
    }

    // else do the old depth-based approach
    auto t0= std::chrono::high_resolution_clock::now();
    auto [baseAsset, quoteAsset] = parseSymbol(pairName);
    if(quoteAsset=="UNKNOWN"){
        std::cout<<"[SIM] parseSymbol => unknown quote for "<<pairName<<"\n";
        return false;
    }
    bool isSell= (quoteAsset=="USDT"||quoteAsset=="BTC");
    std::string sideStr=(isSell?"SELL":"BUY");
    double freeAmt= (isSell
                     ? wallet_->getFreeBalance(baseAsset)
                     : wallet_->getFreeBalance(quoteAsset));
    double desiredQtyBase=0.0;
    if(isSell){
        desiredQtyBase= std::min(freeAmt, volumeLimit_);
        if(desiredQtyBase<=0.0){
            std::cout<<"[SIM] Not enough "<<baseAsset<<" to sell.\n";
            return false;
        }
    } else {
        double bestAsk=(ob.asks.empty()?99999999.0: ob.asks[0].price);
        double maxSpend= volumeLimit_*bestAsk;
        double spend= std::min(freeAmt, maxSpend);
        desiredQtyBase=(bestAsk>0.0? spend/bestAsk:0.0);
        if(desiredQtyBase<=0.0){
            std::cout<<"[SIM] Not enough "<<quoteAsset<<" to buy "<<baseAsset<<"\n";
            return false;
        }
    }
    double filledQty=0.0;
    double cost=0.0;
    const auto& levels=(isSell? ob.bids: ob.asks);
    if(levels.empty()){
        std::cout<<"[SIM] no orderbook levels for "<<pairName<<"\n";
        return false;
    }
    double remaining= desiredQtyBase;
    for(auto& lvl: levels){
        double tradeQty= std::min(remaining,lvl.quantity);
        double tradeCost= tradeQty*lvl.price;
        filledQty += tradeQty;
        cost      += tradeCost;
        remaining-= tradeQty;
        if(remaining<=1e-12) break;
    }
    if(filledQty<=1e-12){
        std::cout<<"[SIM] no fill at all from depth for "<<pairName<<"\n";
        return false;
    }
    double avgPrice= cost/filledQty;
    double fillRatio= filledQty/ desiredQtyBase;
    if(fillRatio< minFillRatio_){
        std::cout<<"[SIM] fillRatio="<<fillRatio<<" < "<<minFillRatio_<<"\n";
        return false;
    }
    double bestPx=0.0;
    if(isSell){
        bestPx=(ob.bids.empty()?0.0: ob.bids[0].price);
    } else {
        bestPx=(ob.asks.empty()?99999999.0: ob.asks[0].price);
    }
    if(bestPx<=0.0){
        std::cout<<"[SIM] bestPx=0 => empty book\n";
        return false;
    }
    double slip= std::fabs(avgPrice- bestPx)/ bestPx;
    if(slip> slippageTolerance_){
        std::cout<<"[SIM] slippage="<<slip<<" > tol="<< slippageTolerance_<<"\n";
        return false;
    }
    double netCostOrProceeds= (isSell
                               ? cost*(1.0- feePercent_)
                               : cost*(1.0+ feePercent_));
    bool ok1=false, ok2=false;
    if(isSell){
        ok1= wallet_->applyChange(tx,baseAsset, -filledQty,0.0);
        ok2= wallet_->applyChange(tx,quoteAsset, netCostOrProceeds,0.0);
    } else {
        ok1= wallet_->applyChange(tx,quoteAsset, -netCostOrProceeds,0.0);
        ok2= wallet_->applyChange(tx,baseAsset, filledQty,0.0);
    }
    if(!ok1|| !ok2){
        std::cout<<"[SIM] wallet applyChange fail\n";
        return false;
    }

    auto t1= std::chrono::high_resolution_clock::now();
    double ms= std::chrono::duration<double,std::milli>(t1 - t0).count();

    std::cout<<"[SIM] "<<sideStr<<" dynamic on "<<pairName
             <<" desiredQty="<< desiredQtyBase
             <<" filledQty="<<filledQty
             <<" avgPrice="<<avgPrice
             <<" costOrProceeds="<< cost
             <<" time="<<ms<<" ms\n";

    logLeg(pairName, sideStr, desiredQtyBase, filledQty,
           fillRatio, slip, ms);
    return true;
}

/**
 * doLegLive => place real market order via executor, apply wallet changes
 */
bool Simulator::doLegLive(WalletTransaction& tx,
                          const std::string& pairName,
                          double desiredQtyBase,
                          bool isSell)
{
    auto t0= std::chrono::high_resolution_clock::now();
    std::string sideStr= (isSell?"SELL":"BUY");

    // 1) place real market order
    OrderSide sideEnum= (isSell? OrderSide::SELL : OrderSide::BUY);
    OrderResult orderRes= executor_->placeMarketOrder(pairName, sideEnum, desiredQtyBase);
    if(!orderRes.success || orderRes.filledQuantity<=0.0){
        std::cout<<"[SIM-LIVE] placeMarketOrder fail or no fill. msg="<<orderRes.message<<"\n";
        return false;
    }
    double fillQtyBase= orderRes.filledQuantity;
    double fillRatio= fillQtyBase/ desiredQtyBase;
    if(fillRatio< minFillRatio_){
        std::cout<<"[SIM-LIVE] fill ratio < "<<minFillRatio_
                 <<" => "<< fillRatio<<"\n";
        return false;
    }
    // fee logic 
    double netCostOrProceeds= orderRes.costOrProceeds;
    // Add or subtract fee?
    if(isSell){
        netCostOrProceeds *= (1.0 - feePercent_);
    } else {
        netCostOrProceeds *= (1.0 + feePercent_);
    }

    auto [baseAsset, quoteAsset]= parseSymbol(pairName);
    bool ok1=false,ok2=false;
    if(isSell){
        ok1= wallet_->applyChange(tx, baseAsset, -fillQtyBase,0.0);
        ok2= wallet_->applyChange(tx, quoteAsset, netCostOrProceeds,0.0);
    } else {
        ok1= wallet_->applyChange(tx, quoteAsset, -netCostOrProceeds,0.0);
        ok2= wallet_->applyChange(tx, baseAsset, fillQtyBase,0.0);
    }
    if(!ok1|| !ok2){
        std::cout<<"[SIM-LIVE] wallet applyChange fail\n";
        return false;
    }

    auto t1= std::chrono::high_resolution_clock::now();
    double ms= std::chrono::duration<double,std::milli>(t1 - t0).count();

    // log
    std::cout<<"[SIM-LIVE] "<<sideStr<<" "<< fillQtyBase
             <<" base on "<<pairName
             <<" avgPrice="<< orderRes.avgPrice
             <<" costOrProceeds="<< orderRes.costOrProceeds
             <<" time="<<ms<<" ms\n";

    logLeg(pairName, sideStr, desiredQtyBase, fillQtyBase,
           fillRatio, /*slip=*/0.0, ms);
    return true;
}

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

// logs the entire 3-leg trade
void Simulator::logTrade(const std::string& path,
                         double startVal,
                         double endVal,
                         double profitPercent)
{
    std::ofstream file(logFileName_, std::ios::app);
    if (!file.is_open()) return;

    auto now= std::chrono::system_clock::now();
    auto now_c= std::chrono::system_clock::to_time_t(now);

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
    static const char* LEG_LOG_FILE="leg_log.csv";
    static bool headerWritten=false;
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lg(logMutex);

    std::ofstream f(LEG_LOG_FILE, std::ios::app);
    if(!f.is_open()) return;

    if(!headerWritten){
        f<<"timestamp,pair,side,requestedQty,filledQty,fillRatio,slippage,latencyMs\n";
        headerWritten=true;
    }
    auto now= std::chrono::system_clock::now();
    auto now_c= std::chrono::system_clock::to_time_t(now);

    f<< std::put_time(std::localtime(&now_c),"%F %T") <<","
      << pairName <<","
      << side <<","
      << requestedQty <<","
      << filledQty <<","
      << fillRatio <<","
      << slipPct <<","
      << latencyMs <<"\n";
}

int Simulator::getTotalTrades() const {
    return totalTrades_;
}

double Simulator::getCumulativeProfit() const {
    return cumulativeProfit_;
}

std::vector<std::string> Simulator::getAssetsForPair(const std::string& pairName) const {
    auto [b,q]= parseSymbol(pairName);
    if(q=="UNKNOWN"){
        return {};
    }
    return {b,q};
}
