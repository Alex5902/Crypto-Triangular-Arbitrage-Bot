#include "engine/triangle_scanner.hpp"
#include "engine/simulator.hpp"
#include "core/orderbook.hpp"
#include <iostream>
#include <fstream>
#include <sstream>
#include <algorithm>
#include <nlohmann/json.hpp>
#include <chrono>
#include <ctime>
#include <iomanip>
#include <curl/curl.h>  // for HTTP fetch

using json = nlohmann::json;

// Turn on reversed edges by default
static bool USE_INVERSE_EDGES = true;
// BFS debug info
static bool DEBUG_BFS = true;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, std::string* out) {
    size_t totalSize = size * nmemb;
    out->append((char*)contents, totalSize);
    return totalSize;
}

TriangleScanner::TriangleScanner()
    : pool_(4)
{
}

void TriangleScanner::setOrderBookManager(OrderBookManager* obm) {
    obm_ = obm;
}

/**
 * Optionally keep: load triangles from file
 */
void TriangleScanner::loadTrianglesFromFile(const std::string& filepath) {
    std::ifstream file(filepath);
    if (!file.is_open()) {
        std::cerr << "Cannot open triangle file " << filepath << std::endl;
        return;
    }

    json j;
    file >> j;
    for (auto& item : j) {
        Triangle tri;
        tri.base = item["base"].get<std::string>();

        for (auto& p : item["path"]) {
            tri.path.push_back(p.get<std::string>());
        }

        // start websockets
        for (const auto& sym : tri.path) {
            if (obm_) {
                obm_->start(sym);
            }
        }

        int idx = (int)triangles_.size();
        triangles_.push_back(tri);
        for (auto& sym : tri.path) {
            symbolToTriangles_[sym].push_back(idx);
        }
    }

    // resize lastProfits_ to match new triangles
    lastProfits_.resize(triangles_.size(), -999.0);

    std::cout << "[FILE] Loaded " << triangles_.size() << " triangle(s)\n";
}

/**
 * fetch /exchangeInfo => BFS => store => subscribe
 */
bool TriangleScanner::loadTrianglesFromBinanceExchangeInfo() {
    if (!obm_) {
        std::cerr << "[DYNAMIC] No OrderBookManager set, can't subscribe.\n";
        return false;
    }

    std::string url = "https://api.binance.com/api/v3/exchangeInfo";
    CURL* curl = curl_easy_init();
    if(!curl) {
        std::cerr << "[DYNAMIC] curl init failed\n";
        return false;
    }
    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    CURLcode res = curl_easy_perform(curl);
    if(res != CURLE_OK) {
        std::cerr << "[DYNAMIC] exchangeInfo curl error: "
                  << curl_easy_strerror(res) << "\n";
        curl_easy_cleanup(curl);
        return false;
    }
    curl_easy_cleanup(curl);

    json j;
    try {
        j = json::parse(response);
    } catch(...) {
        std::cerr << "[DYNAMIC] parse exchangeInfo JSON failed\n";
        return false;
    }
    if(!j.contains("symbols") || !j["symbols"].is_array()) {
        std::cerr << "[DYNAMIC] No 'symbols' array in exchangeInfo\n";
        return false;
    }

    // adjacency: baseAsset -> [ (quoteAsset, "SYMBOL_FWD"), ...]
    std::unordered_map<std::string, std::vector<std::pair<std::string,std::string>>> adjacency;
    adjacency.reserve(2000);

    int count=0;
    for (auto& symObj : j["symbols"]) {
        if (!symObj.contains("symbol") ||
            !symObj.contains("baseAsset")||
            !symObj.contains("quoteAsset")||
            !symObj.contains("status")) continue;

        std::string status = symObj["status"].get<std::string>();
        if (status != "TRADING") continue;

        std::string sSymbol    = symObj["symbol"].get<std::string>();
        std::string sBaseAsset = symObj["baseAsset"].get<std::string>();
        std::string sQuoteAsset= symObj["quoteAsset"].get<std::string>();

        // forward direction
        std::string fwdName = sSymbol + "_FWD";
        adjacency[sBaseAsset].push_back({ sQuoteAsset, fwdName });
        ++count;

        if(USE_INVERSE_EDGES){
            // reverse direction
            std::string revName = sSymbol + "_INV";
            adjacency[sQuoteAsset].push_back({ sBaseAsset, revName });
        }
    }

    std::cout << "[DYNAMIC] Found " << count << " trading pairs.\n";
    if(DEBUG_BFS){
        std::cout << "[BFS-DEBUG] # of assets (adjacency.size()) = "
                  << adjacency.size() << "\n";

        // count total edges
        int edgeTotal=0;
        for(const auto& kv: adjacency){
            edgeTotal += (int)kv.second.size();
        }
        std::cout<<"[BFS-DEBUG] total directed edges="<< edgeTotal <<"\n";
    }

    buildTrianglesBFS(adjacency);

    std::cout << "[DYNAMIC] Created " << triangles_.size()
              << " triangle(s) via BFS.\n";

    lastProfits_.resize(triangles_.size(), -999.0);

    // subscribe to each path
    for (auto& tri : triangles_) {
        for (auto& sym : tri.path) {
            obm_->start(sym);
        }
    }

    return true;
}

/**
 * BFS approach
 */
void TriangleScanner::buildTrianglesBFS(const std::unordered_map<std::string, 
                                    std::vector<std::pair<std::string,std::string>>>& adjacency)
{
    triangles_.clear();
    symbolToTriangles_.clear();

    int cycleCount=0;

    for (auto& kv : adjacency) {
        const std::string& A = kv.first;
        const auto& neighborsA = kv.second;

        for (auto& pairAB : neighborsA) {
            std::string B = pairAB.first;
            std::string symAB = pairAB.second;

            auto itB = adjacency.find(B);
            if (itB == adjacency.end()) continue;

            for (auto& pairBC : itB->second) {
                std::string C = pairBC.first;
                std::string symBC = pairBC.second;

                auto itC = adjacency.find(C);
                if (itC == adjacency.end()) continue;

                for (auto& pairCA : itC->second) {
                    if (pairCA.first == A) {
                        std::string symCA = pairCA.second;
                        ++cycleCount;

                        if(DEBUG_BFS){
                            std::cout<<"[BFS-DEBUG] cycle#"<< cycleCount <<" => "
                                     << A <<"->"<< B <<"->"<< C <<"->"<< A
                                     << "  symbols: "
                                     << symAB <<", "<< symBC <<", "<< symCA <<"\n";
                        }

                        Triangle tri;
                        tri.base = A; 
                        tri.path.push_back(symAB);
                        tri.path.push_back(symBC);
                        tri.path.push_back(symCA);

                        int idx = (int)triangles_.size();
                        triangles_.push_back(tri);

                        symbolToTriangles_[symAB].push_back(idx);
                        symbolToTriangles_[symBC].push_back(idx);
                        symbolToTriangles_[symCA].push_back(idx);
                    }
                }
            }
        }
    }

    if(DEBUG_BFS){
        std::cout<<"[BFS-DEBUG] total cycles found="<< cycleCount <<"\n";
    }
}

static const int TOP_TRIANGLE_LIMIT = 50;

void TriangleScanner::scanTrianglesForSymbol(const std::string& symbol) {
    auto t0 = std::chrono::steady_clock::now();
    if (!obm_) return;

    auto it = symbolToTriangles_.find(symbol);
    if (it == symbolToTriangles_.end()) {
        return;
    }
    const auto& allTris = it->second;

    int limit = std::min<int>((int)allTris.size(), TOP_TRIANGLE_LIMIT);

    std::vector<std::future<double>> futs;
    futs.reserve(limit);
    for (int i=0; i<limit; i++){
        int triIdx = allTris[i];
        futs.push_back(pool_.submit([this, triIdx](){
            return calculateProfit(triangles_[triIdx]);
        }));
    }

    std::vector<double> profits(limit);
    for(int i=0; i<limit; i++){
        profits[i] = futs[i].get();
    }

    double bestProfit= -999.0;
    int bestLocalIdx= -1;
    for(int i=0; i<limit; i++){
        double pf = profits[i];
        if(pf> bestProfit){
            bestProfit= pf;
            bestLocalIdx= i;
        }
    }

    for(int i=0; i<limit; i++){
        int triIdx = allTris[i];
        updateTrianglePriority(triIdx, profits[i]);
    }

    if(bestProfit> minProfitThreshold_ && bestLocalIdx>=0){
        int bestTriIdx = allTris[bestLocalIdx];
        const auto& tri = triangles_[ bestTriIdx ];
        std::cout << "[BEST ROUTE for " << symbol << "] "
                  << tri.path[0] << "->"
                  << tri.path[1] << "->"
                  << tri.path[2] << " => "
                  << bestProfit << "%\n";

        if(simulator_){
            auto ob1= obm_->getOrderBook(tri.path[0]);
            auto ob2= obm_->getOrderBook(tri.path[1]);
            auto ob3= obm_->getOrderBook(tri.path[2]);

            double estProfitUSDT= simulator_->estimateTriangleProfitUSDT(tri, ob1, ob2, ob3);
            if(estProfitUSDT<0.0){
                std::cout<<"[SCAN] Full-triangle => negative => skip\n";
            } else if(estProfitUSDT<2.0){
                std::cout<<"[SCAN] => "<< estProfitUSDT <<" < 2 USDT => skip\n";
            } else {
                std::cout<<"[SIMULATE] => +"<< estProfitUSDT <<" USDT => do real trade.\n";
                simulator_->simulateTradeDepthWithWallet(tri, ob1, ob2, ob3);
                simulator_->printWallet();
            }
        }
    }

    auto t1= std::chrono::steady_clock::now();
    double ms= std::chrono::duration<double,std::milli>(t1 - t0).count();
    std::cout<<"[SCANNER LATENCY] symbol="<< symbol
             <<" took "<< ms <<" ms\n";

    logScanResult(symbol, (int)allTris.size(), bestProfit, ms);
}

/**
 * The big change here is interpret "XXX_INV" as reversed
 */
double TriangleScanner::calculateProfit(const Triangle& tri) {
    if(!obm_) return -999;
    if(tri.path.size()<3) return -999;

    double amount = 1.0;
    double fee = 0.001;

    for(int leg=0; leg<3; leg++){
        const std::string& sym = tri.path[leg];
        // Is reversed?
        bool isReversed = false;
        std::string rawSym = sym;
        if(sym.size()>=4) {
            if(sym.compare(sym.size()-4, 4, "_INV")==0){
                isReversed = true;
                rawSym = sym.substr(0, sym.size()-4);
            }
            else if(sym.compare(sym.size()-4,4,"_FWD")==0){
                rawSym = sym.substr(0, sym.size()-4);
            }
        }

        // get orderbook
        auto ob = obm_->getOrderBook(rawSym);
        if(ob.bids.empty()|| ob.asks.empty()){
            return -999; 
        }
        double bestBid= ob.bids[0].price;
        double bestAsk= ob.asks[0].price;
        if(bestBid<=0.0|| bestAsk<=0.0) return -999;

        if(!isReversed){
            // normal => "sell base" for "quote" at bestBid
            double out = amount * bestBid;
            out = out*(1.0 - fee); 
            amount = out; 
        } else {
            // reversed => "spend quote" to "buy base" at bestAsk
            double baseGained = (amount/bestAsk)*(1.0 - fee);
            amount = baseGained;
        }
    }

    double profitPct = (amount - 1.0)*100.0;
    return profitPct;
}

void TriangleScanner::scanAllSymbolsConcurrently() {
    std::vector<std::string> allSymbols;
    allSymbols.reserve(symbolToTriangles_.size());
    for(auto& kv: symbolToTriangles_){
        allSymbols.push_back(kv.first);
    }

    std::vector<std::future<void>> futs;
    futs.reserve(allSymbols.size());
    for(auto& symbol: allSymbols){
        futs.push_back(pool_.submit([this, &symbol](){
            this->scanTrianglesForSymbol(symbol);
        }));
    }
    for(auto& f : futs){
        f.wait();
    }
}

void TriangleScanner::logScanResult(const std::string& symbol,
                                    int triCount,
                                    double bestProfit,
                                    double latencyMs)
{
    std::lock_guard<std::mutex> lock(scanLogMutex_);
    std::ofstream file("scan_log.csv", std::ios::app);
    if (!file.is_open()) return;

    if (!scanLogHeaderWritten_) {
        file << "timestamp,symbol,triangles_scanned,best_profit,latency_ms\n";
        scanLogHeaderWritten_ = true;
    }

    auto now = std::chrono::system_clock::now();
    auto now_c = std::chrono::system_clock::to_time_t(now);

    file << std::put_time(std::localtime(&now_c), "%F %T") << ","
         << symbol << ","
         << triCount << ","
         << bestProfit << ","
         << latencyMs << "\n";
}

void TriangleScanner::updateTrianglePriority(int triIdx, double profit) {
    std::lock_guard<std::mutex> lk(bestTriMutex_);
    lastProfits_[triIdx] = profit;
    TriPriority item;
    item.profit = profit;
    item.triIdx = triIdx;
    bestTriangles_.push(item);
}

bool TriangleScanner::getBestTriangle(double& outProfit, Triangle& outTri) {
    std::lock_guard<std::mutex> lk(bestTriMutex_);
    while(!bestTriangles_.empty()){
        TriPriority top = bestTriangles_.top();
        double stored = lastProfits_[top.triIdx];
        if(std::fabs(stored - top.profit)<1e-12){
            // up to date
            outProfit = stored;
            outTri    = triangles_[ top.triIdx ];
            return true;
        } else {
            // stale => pop
            bestTriangles_.pop();
        }
    }
    return false;
}

/** 
 * NEW #1: Re-check all triangles in parallel, update bestTriangles_, 
 * optionally produce a sorted list of results above minProfitPct.
 */
void TriangleScanner::rescoreAllTrianglesConcurrently(
    double minProfitPct,
    std::vector<ScoredTriangle>* outSorted)
{
    if(triangles_.empty()) return;

    // concurrency over all triangle indices
    std::vector<std::future<double>> futs;
    futs.reserve(triangles_.size());

    for(size_t i=0; i< triangles_.size(); i++){
        futs.push_back(pool_.submit([this, i](){
            return calculateProfit(triangles_[i]);
        }));
    }

    // We'll store in a local vector
    std::vector<double> profits(triangles_.size());

    // gather
    for(size_t i=0; i< futs.size(); i++){
        profits[i] = futs[i].get();
    }

    // now update bestTriangles_ (threadsafe)
    {
        std::lock_guard<std::mutex> lk(bestTriMutex_);
        // clear out old queue
        while(!bestTriangles_.empty()) bestTriangles_.pop();
        // push new
        for(size_t i=0; i< profits.size(); i++){
            double pf = profits[i];
            lastProfits_[i] = pf; // store
            if(pf >= minProfitPct){
                TriPriority item;
                item.profit = pf;
                item.triIdx = (int)i;
                bestTriangles_.push(item);
            }
        }
    }

    // if user wants a sorted list, produce it
    if(outSorted){
        outSorted->clear();
        outSorted->reserve(triangles_.size());
        for(size_t i=0; i< profits.size(); i++){
            double pf = profits[i];
            if(pf >= minProfitPct){
                ScoredTriangle sc;
                sc.triIdx  = (int)i;
                sc.profit  = pf;
                sc.netUSDT = 0.0; // if you want to do a deeper USDT sim, do it here
                outSorted->push_back(sc);
            }
        }
        // sort descending
        std::sort(outSorted->begin(), outSorted->end(),
                  [](auto&a,auto&b){return a.profit> b.profit;});
    }

    std::cout << "[RESCORE] updated all " << triangles_.size()
              << " triangles. top queue size=" << bestTriangles_.size()
              << ", minProfit=" << minProfitPct << "\n";
}

/**
 * NEW #2: Export top triangles from bestTriangles_ to CSV
 */
void TriangleScanner::exportTopTrianglesCSV(const std::string& filename,
                                            int topN,
                                            double minProfitPct)
{
    std::vector<ScoredTriangle> results;
    // We'll build a local sorted array from bestTriangles_
    {
        std::lock_guard<std::mutex> lk(bestTriMutex_);

        // make a temporary copy of the queue
        std::priority_queue<TriPriority> tmp = bestTriangles_;
        while(!tmp.empty()){
            TriPriority top = tmp.top();
            tmp.pop();
            if(top.profit< minProfitPct) break; // or continue
            ScoredTriangle sc;
            sc.triIdx  = top.triIdx;
            sc.profit  = top.profit;
            sc.netUSDT = 0.0; // if you want to do a deeper USDT sim, you could
            results.push_back(sc);
        }
    }

    // now results is unsorted descending by queue order, but let's ensure it:
    std::sort(results.begin(), results.end(),
              [](auto&a, auto&b){return a.profit> b.profit;});

    if((int)results.size()> topN){
        results.resize(topN);
    }

    // open CSV
    std::ofstream fs(filename, std::ios::app);
    if(!fs.is_open()){
        std::cerr<<"[EXPORT] Could not open "<< filename <<"\n";
        return;
    }
    // optional header
    static bool firstWrite=true;
    if(firstWrite){
        fs << "timestamp,rank,triIdx,profitPct,path\n";
        firstWrite=false;
    }

    auto now = std::chrono::system_clock::now();
    auto now_c= std::chrono::system_clock::to_time_t(now);
    std::string nowStr = std::string(std::ctime(&now_c));
    // remove trailing newline
    if(!nowStr.empty() && nowStr.back()=='\n') nowStr.pop_back();

    int rank=1;
    for(auto& sc: results){
        auto& tri = triangles_[ sc.triIdx ];
        std::stringstream pathStr;
        for(size_t i=0; i< tri.path.size(); i++){
            if(i>0) pathStr<<"->";
            pathStr<< tri.path[i];
        }
        fs << nowStr <<","<< rank <<","<< sc.triIdx
           <<","<< sc.profit
           <<","<< pathStr.str() <<"\n";
        rank++;
    }
    fs.close();

    std::cout<<"[EXPORT] wrote "<< results.size() <<" triangles to "<< filename <<"\n";
}
