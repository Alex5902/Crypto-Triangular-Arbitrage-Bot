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

/**
 *  Constructor: We'll treat `volumeLimit` as a fraction of your free balance
 *  i.e., if volumeLimit=0.5 => use 50% of free wallet asset for trades.
 */
Simulator::Simulator(const std::string& logFileName,
                     double feePercent,
                     double slippageTolerance,
                     double volumeLimit,     // We'll reinterpret as maxFractionPerTrade
                     double minFillRatio,
                     Wallet* sharedWallet,
                     IExchangeExecutor* executor,
                     double minProfitUSDT)
  : logFileName_(logFileName)
  , feePercent_(feePercent)
  , slippageTolerance_(slippageTolerance)
  , maxFractionPerTrade_(volumeLimit)   // rename internally
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

    // Load symbol filters if available
    loadSymbolFilters("config/symbol_filters.json");
}

/**
 * Load minNotional, minQty per symbol from JSON file (symbol_filters.json).
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
        std::cerr << "[SIM] Error parsing " << path << ": " << e.what() << "\n";
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
        // fallback if no data
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

    if (quantityBase < filt.minQty) {
        std::cout << "[FILTER] " << symbol << ": quantityBase="
                  << quantityBase << " < minQty=" << filt.minQty << "\n";
        return false;
    }
    if (notional < filt.minNotional) {
        std::cout << "[FILTER] " << symbol << ": notional="
                  << notional << " < minNotional=" << filt.minNotional << "\n";
        return false;
    }
    return true;
}

/**
 * simulateTradeDepthWithWallet:
 *  1) estimateTriangleProfitUSDT => checks if netProfit > minProfitUSDT_
 *  2) if pass => lock assets, do doLeg(...) for each leg
 */
bool Simulator::simulateTradeDepthWithWallet(const Triangle& tri,
                                             const OrderBookData& ob1,
                                             const OrderBookData& ob2,
                                             const OrderBookData& ob3)
{
    // measure starting USDT
    double b1 = (ob1.bids.empty() ? 0.0 : ob1.bids[0].price);
    double b2 = (ob2.bids.empty() ? 0.0 : ob2.bids[0].price);
    double b3 = (ob3.bids.empty() ? 0.0 : ob3.bids[0].price);

    double oldValUSDT = wallet_->getFreeBalance("BTC") * b1
                      + wallet_->getFreeBalance("ETH") * b2
                      + wallet_->getFreeBalance("USDT");

    // estimate final USDT
    double estProfitUSDT = estimateTriangleProfitUSDT(tri, ob1, ob2, ob3);
    if (estProfitUSDT < 0.0) {
        std::cout << "[SIM] Pre-check => unprofitable or fill fail. Skip.\n";
        return false;
    }
    if (estProfitUSDT < minProfitUSDT_) {
        std::cout << "[SIM] Pre-check => estProfit=" << estProfitUSDT
                  << " < min=" << minProfitUSDT_ << " => skip\n";
        return false;
    }

    // lock all relevant assets
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

    // do the 3 legs
    auto tx = wallet_->beginTransaction();
    if (!doLeg(tx, tri.path[0], ob1)) {
        wallet_->rollbackTransaction(tx);
        return false;
    }
    if (!doLeg(tx, tri.path[1], ob2)) {
        wallet_->rollbackTransaction(tx);
        return false;
    }
    if (!doLeg(tx, tri.path[2], ob3)) {
        wallet_->rollbackTransaction(tx);
        return false;
    }
    wallet_->commitTransaction(tx);

    double newValUSDT = wallet_->getFreeBalance("BTC") * b3
                      + wallet_->getFreeBalance("ETH") * b2
                      + wallet_->getFreeBalance("USDT");
    double absoluteProfit = (newValUSDT - oldValUSDT);
    double profitPercent  = (oldValUSDT > 0.0 ? (absoluteProfit/oldValUSDT)*100.0 : 0.0);

    // Log
    std::stringstream ps;
    for (size_t i=0; i< tri.path.size(); i++){
        if(i>0) ps<<"->";
        ps << tri.path[i];
    }
    logTrade(ps.str(), oldValUSDT, newValUSDT, profitPercent);

    if(absoluteProfit > -1e-14){
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
 * estimateTriangleProfitUSDT => multi-leg dry-run in a "fake wallet" with adaptive sizing
 */
double Simulator::estimateTriangleProfitUSDT(const Triangle& tri,
                                             const OrderBookData& ob1,
                                             const OrderBookData& ob2,
                                             const OrderBookData& ob3)
{
    double b1 = (ob1.bids.empty()?0.0:ob1.bids[0].price);
    double b2 = (ob2.bids.empty()?0.0:ob2.bids[0].price);
    double b3 = (ob3.bids.empty()?0.0:ob3.bids[0].price);

    double startValUSDT = wallet_->getFreeBalance("BTC") * b1
                        + wallet_->getFreeBalance("ETH") * b2
                        + wallet_->getFreeBalance("USDT");

    // fake wallet
    double fakeBTC  = wallet_->getFreeBalance("BTC");
    double fakeETH  = wallet_->getFreeBalance("ETH");
    double fakeUSDT = wallet_->getFreeBalance("USDT");

    auto simulateLeg = [&](const std::string& pairName, const OrderBookData& ob)->bool {
        auto [baseAsset, quoteAsset] = parseSymbol(pairName);
        if(quoteAsset=="UNKNOWN") return false;

        bool isSell = (quoteAsset=="USDT" || quoteAsset=="BTC" || quoteAsset=="BUSD" || quoteAsset=="ETH");

        // pick best price for notional check
        double bestPx=0.0;
        if(isSell && !ob.bids.empty()) bestPx= ob.bids[0].price;
        else if(!isSell && !ob.asks.empty()) bestPx= ob.asks[0].price;
        if(bestPx<=0.0) return false;

        // figure out how much of the wallet to use => fraction
        double fraction= maxFractionPerTrade_; // 0.0..1.0
        double freeAmt=0.0;
        double desiredQtyBase=0.0;

        if(isSell){
            // we have base
            if(baseAsset=="BTC") freeAmt=fakeBTC;
            else if(baseAsset=="ETH") freeAmt=fakeETH;
            if(freeAmt<=0.0) return false;

            double rawQty= freeAmt * fraction;
            if(rawQty<=0.0) return false;

            desiredQtyBase= rawQty;
        } else {
            // we buy base => we have quote
            if(quoteAsset=="USDT") freeAmt=fakeUSDT;
            else if(quoteAsset=="BTC") freeAmt=fakeBTC;
            else if(quoteAsset=="ETH") freeAmt=fakeETH;
            if(freeAmt<=0.0) return false;

            double rawSpend= freeAmt * fraction; 
            if(rawSpend<=0.0) return false;

            // convert spend in quote to base
            desiredQtyBase= rawSpend / bestPx;
        }

        // check binance filters
        if(!passesExchangeFilters(pairName, desiredQtyBase, bestPx)){
            return false;
        }

        // do a partial fill across levels
        double filled=0.0, cost=0.0;
        const auto& levels= (isSell ? ob.bids : ob.asks);
        double remain= desiredQtyBase;
        for(auto &lvl: levels){
            double tradeQty= std::min(remain, lvl.quantity);
            double tradeCost= tradeQty * lvl.price;
            filled += tradeQty;
            cost   += tradeCost;
            remain -= tradeQty;
            if(remain<=1e-12) break;
        }
        if(filled<=1e-12) return false;

        double fillRatio= filled/ desiredQtyBase;
        if(fillRatio< minFillRatio_) return false;

        double avgPx= cost/filled;
        double slip= std::fabs(avgPx- bestPx)/ bestPx;
        if(slip> slippageTolerance_) return false;

        // apply fee
        if(isSell){
            double netProceeds= cost*(1.0 - feePercent_);
            // update fake
            if(baseAsset=="BTC") fakeBTC-= filled;
            else if(baseAsset=="ETH") fakeETH-= filled;

            if(quoteAsset=="USDT") fakeUSDT+= netProceeds;
            else if(quoteAsset=="BTC") fakeBTC+= netProceeds;
            else if(quoteAsset=="ETH") fakeETH+= netProceeds;
        } else {
            double netCost= cost*(1.0 + feePercent_);
            if(quoteAsset=="USDT") fakeUSDT-= netCost;
            else if(quoteAsset=="BTC") fakeBTC-= netCost;
            else if(quoteAsset=="ETH") fakeETH-= netCost;

            if(baseAsset=="BTC") fakeBTC+= filled;
            else if(baseAsset=="ETH") fakeETH+= filled;
        }
        return true;
    };

    if(!simulateLeg(tri.path[0], ob1)) return -1.0;
    if(!simulateLeg(tri.path[1], ob2)) return -1.0;
    if(!simulateLeg(tri.path[2], ob3)) return -1.0;

    double finalValUSDT= fakeUSDT + (fakeBTC*b3) + (fakeETH*b2);
    double netProfit= finalValUSDT- startValUSDT;
    return netProfit;
}

/**
 * doLeg => local simulation of a single leg with adaptive fraction approach
 */
bool Simulator::doLeg(WalletTransaction& tx,
                      const std::string& pairName,
                      const OrderBookData& ob)
{
    if(liveMode_){
        // parse side, doLegLive
        auto [baseAsset, quoteAsset] = parseSymbol(pairName);
        if(quoteAsset=="UNKNOWN"){
            std::cout<<"[SIM-LIVE] unknown quote for "<<pairName<<"\n";
            return false;
        }
        bool isSell= (quoteAsset=="USDT"||quoteAsset=="BTC"||quoteAsset=="BUSD"||quoteAsset=="ETH");
        double freeAmt= (isSell
                         ? wallet_->getFreeBalance(baseAsset)
                         : wallet_->getFreeBalance(quoteAsset));
        if(freeAmt<=0.0){
            std::cout<<"[SIM-LIVE] not enough "<<(isSell? baseAsset: quoteAsset)<<"\n";
            return false;
        }

        // fraction-based sizing
        double fraction= maxFractionPerTrade_;
        double used= freeAmt * fraction;
        if(used<=0.0){
            std::cout<<"[SIM-LIVE] fraction-based quantity=0?\n";
            return false;
        }

        double desiredQtyBase=0.0;
        if(isSell){
            desiredQtyBase= used; 
        } else {
            double bestAsk= (ob.asks.empty()?99999999.0 : ob.asks[0].price);
            desiredQtyBase= used / bestAsk;
        }
        if(desiredQtyBase<=0.0){
            std::cout<<"[SIM-LIVE] can't calc desiredQtyBase\n";
            return false;
        }
        return doLegLive(tx, pairName, desiredQtyBase, isSell);
    }

    // local sim logic
    auto t0= std::chrono::high_resolution_clock::now();
    auto [baseAsset, quoteAsset]= parseSymbol(pairName);
    if(quoteAsset=="UNKNOWN"){
        std::cout<<"[SIM] parseSymbol => unknown quote for "<<pairName<<"\n";
        return false;
    }
    bool isSell= (quoteAsset=="USDT"||quoteAsset=="BTC"||quoteAsset=="BUSD"||quoteAsset=="ETH");
    std::string sideStr= (isSell?"SELL":"BUY");

    double freeAmt= (isSell
                     ? wallet_->getFreeBalance(baseAsset)
                     : wallet_->getFreeBalance(quoteAsset));
    if(freeAmt<=0.0){
        std::cout<<"[SIM] not enough "<<(isSell? baseAsset: quoteAsset)<<"\n";
        return false;
    }

    double fraction= maxFractionPerTrade_;
    double used= freeAmt * fraction;
    if(used<=0.0){
        std::cout<<"[SIM] fraction-based =0?\n";
        return false;
    }

    double bestPx=0.0;
    if(isSell && !ob.bids.empty()) bestPx= ob.bids[0].price;
    else if(!isSell && !ob.asks.empty()) bestPx= ob.asks[0].price;
    if(bestPx<=0.0){
        std::cout<<"[SIM] no bestPx\n";
        return false;
    }

    double desiredQtyBase= (isSell ? used : (used / bestPx));
    if(desiredQtyBase<=0.0){
        std::cout<<"[SIM] desiredQtyBase=0\n";
        return false;
    }

    // check filters
    if(!passesExchangeFilters(pairName, desiredQtyBase, bestPx)){
        return false;
    }

    // partial depth fill
    double filled=0.0, cost=0.0;
    const auto& levels= (isSell ? ob.bids : ob.asks);
    if(levels.empty()){
        std::cout<<"[SIM] no orderbook levels\n";
        return false;
    }

    double remain= desiredQtyBase;
    for(auto &lvl: levels){
        double tradeQty= std::min(remain, lvl.quantity);
        double tradeCost= tradeQty* lvl.price;
        filled+= tradeQty;
        cost  += tradeCost;
        remain-= tradeQty;
        if(remain<=1e-12) break;
    }
    if(filled<=1e-12){
        std::cout<<"[SIM] no fill at all\n";
        return false;
    }

    double avgPrice= cost/ filled;
    double fillRatio= filled/ desiredQtyBase;
    if(fillRatio< minFillRatio_){
        std::cout<<"[SIM] fillRatio="<<fillRatio<<" < "<<minFillRatio_<<"\n";
        return false;
    }

    double slip= std::fabs(avgPrice - bestPx)/ bestPx;
    if(slip> slippageTolerance_){
        std::cout<<"[SIM] slip="<<slip<<" > tol="<<slippageTolerance_<<"\n";
        return false;
    }

    double netCostOrProceeds= (isSell
                               ? cost* (1.0- feePercent_)
                               : cost* (1.0+ feePercent_));

    bool ok1=false, ok2=false;
    if(isSell){
        ok1= wallet_->applyChange(tx, baseAsset, -filled, 0.0);
        ok2= wallet_->applyChange(tx, quoteAsset, netCostOrProceeds, 0.0);
    } else {
        ok1= wallet_->applyChange(tx, quoteAsset, -netCostOrProceeds, 0.0);
        ok2= wallet_->applyChange(tx, baseAsset, filled, 0.0);
    }
    if(!ok1 || !ok2){
        std::cout<<"[SIM] wallet applyChange fail\n";
        return false;
    }

    auto t1= std::chrono::high_resolution_clock::now();
    double ms= std::chrono::duration<double,std::milli>(t1 - t0).count();

    std::cout<<"[SIM] "<< sideStr <<" on "<< pairName
             <<" fraction="<< fraction
             <<" desiredQty="<< desiredQtyBase
             <<" filled="<< filled
             <<" avgPrice="<< avgPrice
             <<" cost="<< cost
             <<" time="<< ms <<" ms\n";

    logLeg(pairName, sideStr, desiredQtyBase, filled, fillRatio, slip, ms);
    return true;
}

/**
 * doLegLive => place a real MARKET order via the executor, then update wallet with fill result
 */
bool Simulator::doLegLive(WalletTransaction& tx,
                          const std::string& pairName,
                          double desiredQtyBase,
                          bool isSell)
{
    auto t0= std::chrono::high_resolution_clock::now();
    std::string sideStr= (isSell? "SELL":"BUY");

    // We'll guess a price of 30000 for filters, or fetch from global OB.
    double approximatePrice=30000.0;
    if(!passesExchangeFilters(pairName, desiredQtyBase, approximatePrice)){
        std::cout<<"[SIM-LIVE] fails exchange filters\n";
        return false;
    }

    OrderSide sideEnum= (isSell? OrderSide::SELL : OrderSide::BUY);
    OrderResult orderRes= executor_->placeMarketOrder(pairName, sideEnum, desiredQtyBase);
    if(!orderRes.success || orderRes.filledQuantity<=0.0){
        std::cout<<"[SIM-LIVE] placeMarketOrder fail: "<< orderRes.message<<"\n";
        return false;
    }

    double fillQtyBase= orderRes.filledQuantity;
    double fillRatio= fillQtyBase/ desiredQtyBase;
    if(fillRatio< minFillRatio_){
        std::cout<<"[SIM-LIVE] fillRatio="<< fillRatio
                 <<" < "<< minFillRatio_ <<"\n";
        return false;
    }

    // fees
    double netCostOrProceeds= orderRes.costOrProceeds;
    if(isSell){
        netCostOrProceeds*= (1.0- feePercent_);
    } else {
        netCostOrProceeds*= (1.0+ feePercent_);
    }

    auto [baseAsset, quoteAsset]= parseSymbol(pairName);
    bool ok1=false, ok2=false;
    if(isSell){
        ok1= wallet_->applyChange(tx, baseAsset, -fillQtyBase, 0.0);
        ok2= wallet_->applyChange(tx, quoteAsset, netCostOrProceeds, 0.0);
    } else {
        ok1= wallet_->applyChange(tx, quoteAsset, -netCostOrProceeds, 0.0);
        ok2= wallet_->applyChange(tx, baseAsset, fillQtyBase, 0.0);
    }
    if(!ok1 || !ok2){
        std::cout<<"[SIM-LIVE] wallet applyChange fail\n";
        return false;
    }

    auto t1= std::chrono::high_resolution_clock::now();
    double ms= std::chrono::duration<double,std::milli>(t1 - t0).count();
    std::cout<<"[SIM-LIVE] "<< sideStr <<" "<< fillQtyBase
             <<" base on "<< pairName
             <<" costOrProceeds="<< orderRes.costOrProceeds
             <<" time="<< ms <<" ms\n";

    logLeg(pairName, sideStr, desiredQtyBase, fillQtyBase,
           fillRatio, 0.0, ms);
    return true;
}

/**
 * optional older stub
 */
double Simulator::simulateTrade(const Triangle&,
                                double,
                                double, double,
                                double, double,
                                double, double)
{
    // no-op
    return 0.0;
}

void Simulator::printWallet() const {
    wallet_->printAll();
}

// logs entire 3-leg trade
void Simulator::logTrade(const std::string& path,
                         double startVal,
                         double endVal,
                         double profitPercent)
{
    std::ofstream file(logFileName_, std::ios::app);
    if(!file.is_open()) return;

    auto now= std::chrono::system_clock::now();
    auto now_c= std::chrono::system_clock::to_time_t(now);

    file<< std::put_time(std::localtime(&now_c), "%F %T") <<","
        << path     <<","
        << startVal <<","
        << endVal   <<","
        << profitPercent <<"\n";
}

// logs each leg detail
void Simulator::logLeg(const std::string& pairName,
                       const std::string& side,
                       double requestedQty,
                       double filledQty,
                       double fillRatio,
                       double slipPct,
                       double latencyMs)
{
    static const char* LEG_LOG_FILE= "leg_log.csv";
    static bool headerWritten= false;
    static std::mutex logMutex;
    std::lock_guard<std::mutex> lg(logMutex);

    std::ofstream f(LEG_LOG_FILE, std::ios::app);
    if(!f.is_open()) return;

    if(!headerWritten){
        f<<"timestamp,pair,side,requestedQty,filledQty,fillRatio,slippage,latencyMs\n";
        headerWritten= true;
    }
    auto now= std::chrono::system_clock::now();
    auto now_c= std::chrono::system_clock::to_time_t(now);

    f<< std::put_time(std::localtime(&now_c),"%F %T") <<","
      << pairName   <<","
      << side       <<","
      << requestedQty <<","
      << filledQty  <<","
      << fillRatio  <<","
      << slipPct    <<","
      << latencyMs  <<"\n";
}

int Simulator::getTotalTrades() const {
    return totalTrades_;
}

double Simulator::getCumulativeProfit() const {
    return cumulativeProfit_;
}

std::vector<std::string> Simulator::getAssetsForPair(const std::string& pairName) const {
    auto [b, q]= parseSymbol(pairName);
    if(q=="UNKNOWN"){
        return {};
    }
    return {b, q};
}
