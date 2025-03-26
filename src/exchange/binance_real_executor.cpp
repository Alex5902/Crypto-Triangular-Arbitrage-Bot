#include "exchange/binance_real_executor.hpp"
#include <openssl/hmac.h>   // for HMAC_SHA256
#include <openssl/evp.h>
#include <chrono>
#include <ctime>
#include <sstream>
#include <iomanip>
#include <stdexcept>
#include <curl/curl.h>
#include <nlohmann/json.hpp>
#include "core/orderbook.hpp"
#include <iostream>
#include <thread>

using json = nlohmann::json;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

// static throttle lock
std::mutex BinanceRealExecutor::throttleMutex_{};

// constructor
BinanceRealExecutor::BinanceRealExecutor(const std::string& apiKey,
                                         const std::string& secretKey,
                                         const std::string& baseUrl,
                                         OrderBookManager* obm)
  : apiKey_(apiKey)
  , secretKey_(secretKey)
  , baseUrl_(baseUrl)
  , obm_(obm)
{
    // Optionally init curl globally
    curl_global_init(CURL_GLOBAL_DEFAULT);

    // Initialize times, token bucket to full
    lastRefillRequests_ = std::chrono::steady_clock::now();
    requestTokens_      = (double)maxRequestsPerMinute_;

    currentSecStart_ = std::chrono::steady_clock::now();
    orderCountInCurrentSec_ = 0;
}

/**
 * placeMarketOrder => throttle as an "order" call
 */
OrderResult BinanceRealExecutor::placeMarketOrder(const std::string& symbol,
                                                  OrderSide side,
                                                  double quantityBase)
{
    // Throttle
    throttleRequest(/*isOrder=*/true);

    OrderResult res;
    res.success = false;
    res.filledQuantity = 0.0;
    res.avgPrice = 0.0;
    res.costOrProceeds = 0.0;
    res.message = "";

    std::string sideStr = (side == OrderSide::BUY) ? "BUY" : "SELL";
    std::ostringstream qtySs;
    qtySs << std::fixed << std::setprecision(8) << quantityBase;

    long nowMs = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    std::ostringstream qs;
    qs << "symbol=" << symbol
       << "&side=" << sideStr
       << "&type=MARKET"
       << "&quantity=" << qtySs.str()
       << "&recvWindow=5000"
       << "&timestamp=" << nowMs;

    std::string queryString = qs.str();
    std::string signature   = signQueryString(queryString);
    queryString += "&signature=" + signature;

    // do POST
    std::string endpoint = "/api/v3/order";
    std::string response = httpRequest("POST", endpoint, queryString);

    if (response.empty()) {
        res.message = "Empty response from server";
        return res;
    }

    json j;
    try {
        j = json::parse(response);
    } catch(...) {
        res.message = "Parse error: " + response;
        return res;
    }

    if (j.contains("code") && j["code"].is_number()) {
        res.message = "Binance error code=" + std::to_string(j["code"].get<int>())
                      + " msg=" + j.value("msg","unknown");
        return res;
    }

    // success
    res.success = true;
    double executedQty = std::stod( j.value("executedQty","0.0") );
    double cummQuote   = std::stod( j.value("cummulativeQuoteQty","0.0") );
    res.filledQuantity = executedQty;
    if (executedQty > 0.0) {
        double avgPx = cummQuote / executedQty;
        res.avgPrice = avgPx;
        if (side == OrderSide::SELL) {
            res.costOrProceeds = cummQuote;
        } else {
            res.costOrProceeds = cummQuote;
        }
    }
    res.message = "Order OK";
    return res;
}

/**
 * getOrderBookSnapshot => throttle as a general request
 */
OrderBookData BinanceRealExecutor::getOrderBookSnapshot(const std::string& symbol)
{
    // if we had a real REST call, we'd do throttleRequest(false) here
    // but you're using an internal OrderBookManager, so let's do minimal
    // still we can treat it as 1 request for safety:
    throttleRequest(/*isOrder=*/false);

    if (!obm_) {
        std::cerr << "[REAL] No OrderBookManager => returning empty OB\n";
        return OrderBookData{}; // empty
    }
    return obm_->getOrderBook(symbol);
}

/**
 * signQueryString => HMAC-SHA256
 */
std::string BinanceRealExecutor::signQueryString(const std::string& query) const {
    unsigned char* hmac_result = nullptr;
    hmac_result = HMAC(EVP_sha256(),
                       secretKey_.c_str(), secretKey_.size(),
                       (unsigned char*)query.c_str(), query.size(),
                       NULL, NULL);

    std::ostringstream hex_stream;
    for (int i = 0; i < 32; i++) {
        hex_stream << std::hex << std::setw(2) << std::setfill('0')
                   << (int)hmac_result[i];
    }
    return hex_stream.str();
}

/**
 * Minimal http request with libcurl, now calling throttleRequest if we want
 * every HTTP call to be throttled. (We'll do that externally for clarity.)
 */
std::string BinanceRealExecutor::httpRequest(const std::string& method,
                                             const std::string& endpoint,
                                             const std::string& queryString)
{
    // We'll assume placeMarketOrder and getOB calls the throttleRequest
    // so we won't double throttle here. If you prefer, you can do a general
    // throttleRequest(false) in here as well.

    std::string url = baseUrl_ + endpoint;
    if (method == "GET" && !queryString.empty()) {
        url += "?" + queryString;
    }

    CURL* curl = curl_easy_init();
    if(!curl) {
        return "";
    }

    std::string readBuffer;
    struct curl_slist* chunk = nullptr;
    chunk = curl_slist_append(chunk, ("X-MBX-APIKEY: " + apiKey_).c_str());
    chunk = curl_slist_append(chunk, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    if (method == "POST") {
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, queryString.c_str());
    } else if (method == "GET") {
        // do nothing
    }

    CURLcode ret = curl_easy_perform(curl);
    if(ret != CURLE_OK) {
        curl_easy_cleanup(curl);
        return "";
    }
    curl_easy_cleanup(curl);
    return readBuffer;
}

/**
 * throttleRequest => main rate-limiting logic:
 *  - Token bucket approach for requests 
 *  - Check short-burst orders per second
 */
void BinanceRealExecutor::throttleRequest(bool isOrder)
{
    std::lock_guard<std::mutex> lg(throttleMutex_);

    // 1) Refill requestTokens_ if needed
    refillRequestTokens();

    // 2) If it's an order, check ordersPerSec limit
    if(isOrder){
        resetOrderCounterIfNewSecond();
        // if we already have e.g. 10 orders in this second => wait
        while(orderCountInCurrentSec_ >= maxOrdersPerSec_){
            // wait 100 ms, or until next second
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            resetOrderCounterIfNewSecond();
        }
        // now we can increment orderCount
        orderCountInCurrentSec_ += 1;
    }

    // 3) For general requests or orders, consume 1 token from requestTokens_
    while(requestTokens_ < 1.0){
        // we have 0 tokens => wait for next refill
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        refillRequestTokens();
    }
    requestTokens_ -= 1.0;
}

/**
 * refillRequestTokens => each minute we restore up to maxRequestsPerMinute_ tokens
 * Implementation uses real time to measure how many tokens to add back
 */
void BinanceRealExecutor::refillRequestTokens()
{
    auto now = std::chrono::steady_clock::now();
    double secondsElapsed = std::chrono::duration<double>(now - lastRefillRequests_).count();

    // each minute we get maxRequestsPerMinute_ new tokens
    // so each second => maxRequestsPerMinute_ / 60 tokens
    double tokensPerSecond = (double)maxRequestsPerMinute_ / 60.0;
    double tokensToAdd     = tokensPerSecond * secondsElapsed;

    if(tokensToAdd >= 1.0){
        requestTokens_ = std::min(
            (double)maxRequestsPerMinute_,
            requestTokens_ + tokensToAdd
        );
        lastRefillRequests_ = now;
    }
}

/**
 * resetOrderCounterIfNewSecond => if a new second has begun, reset 
 */
void BinanceRealExecutor::resetOrderCounterIfNewSecond()
{
    auto now = std::chrono::steady_clock::now();
    double msElapsed = std::chrono::duration<double, std::milli>(now - currentSecStart_).count();
    if(msElapsed >= 1000.0){
        // new second
        currentSecStart_ = now;
        orderCountInCurrentSec_ = 0;
    }
}
