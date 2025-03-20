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

using json = nlohmann::json;

static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

BinanceRealExecutor::BinanceRealExecutor(const std::string& apiKey,
                                         const std::string& secretKey,
                                         const std::string& baseUrl)
  : apiKey_(apiKey)
  , secretKey_(secretKey)
  , baseUrl_(baseUrl)
{
    // Optionally init curl global here
    curl_global_init(CURL_GLOBAL_DEFAULT);
}

OrderResult BinanceRealExecutor::placeMarketOrder(const std::string& symbol,
                                                  OrderSide side,
                                                  double quantityBase)
{
    OrderResult res;
    res.success = false;
    res.filledQuantity = 0.0;
    res.avgPrice = 0.0;
    res.costOrProceeds = 0.0;
    res.message = "";

    // We’ll place a MARKET order on /api/v3/order
    // For testnet, we append `recvWindow=5000` & timestamp & signature
    // We also do a simplistic assumption that the order is fully filled instantly.

    // The side: “BUY” or “SELL”
    std::string sideStr = (side == OrderSide::BUY) ? "BUY" : "SELL";

    // quantity => to string with some fixed precision
    std::ostringstream qtySs;
    qtySs << std::fixed << std::setprecision(8) << quantityBase; 

    long nowMs = (long)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()
    ).count();

    // build query
    std::ostringstream qs;
    qs << "symbol=" << symbol
       << "&side=" << sideStr
       << "&type=MARKET"
       << "&quantity=" << qtySs.str()
       << "&recvWindow=5000"
       << "&timestamp=" << nowMs;

    // sign it
    std::string queryString = qs.str();
    std::string signature = signQueryString(queryString);
    queryString += "&signature=" + signature;

    // do POST
    std::string endpoint = "/api/v3/order";
    std::string response = httpRequest("POST", endpoint, queryString);

    if (response.empty()) {
        res.message = "Empty response from server";
        return res;
    }

    // parse JSON
    json j;
    try {
        j = json::parse(response);
    } catch(...) {
        res.message = "Parse error: " + response;
        return res;
    }

    if (j.contains("code") && j["code"].is_number()) {
        // an error from binance
        res.message = "Binance error code=" + std::to_string(j["code"].get<int>())
                      + " msg=" + j.value("msg","unknown");
        return res;
    }

    // If success
    res.success = true;
    // We can check “executedQty” and “cummulativeQuoteQty”
    double executedQty = std::stod( j.value("executedQty","0.0") );
    double cummQuote   = std::stod( j.value("cummulativeQuoteQty","0.0") );

    res.filledQuantity = executedQty;
    if (executedQty > 0.0) {
        // average price
        double avgPx = cummQuote / executedQty;
        res.avgPrice = avgPx;
        // cost or proceeds
        if (side == OrderSide::SELL) {
            res.costOrProceeds = cummQuote; // total quote received
        } else {
            res.costOrProceeds = cummQuote; // total quote spent
        }
    }
    // message
    res.message = "Order OK";
    return res;
}

// sign with HMAC SHA256
std::string BinanceRealExecutor::signQueryString(const std::string& query) const {
    unsigned char* hmac_result = nullptr;
    hmac_result = HMAC(EVP_sha256(),
                       secretKey_.c_str(), secretKey_.size(),
                       (unsigned char*)query.c_str(), query.size(),
                       NULL, NULL);

    // convert to hex
    std::ostringstream hex_stream;
    for (int i=0; i<32; i++) {
        hex_stream << std::hex << std::setw(2) << std::setfill('0')
                   << (int)hmac_result[i];
    }
    return hex_stream.str();
}

// minimal http request with libcurl
std::string BinanceRealExecutor::httpRequest(const std::string& method,
                                             const std::string& endpoint,
                                             const std::string& queryString)
{
    std::string url = baseUrl_ + endpoint;
    if (method == "GET" && !queryString.empty()) {
        url += "?" + queryString;
    }

    CURL* curl = curl_easy_init();
    if(!curl) {
        return "";
    }

    std::string readBuffer;
    struct curl_slist *chunk = nullptr;
    // set headers
    chunk = curl_slist_append(chunk, ("X-MBX-APIKEY: " + apiKey_).c_str());
    chunk = curl_slist_append(chunk, "Content-Type: application/x-www-form-urlencoded");

    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, chunk);
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &readBuffer);

    if (method == "POST") {
        // we do post
        curl_easy_setopt(curl, CURLOPT_POST, 1L);
        curl_easy_setopt(curl, CURLOPT_POSTFIELDS, queryString.c_str());
    } else if (method == "GET") {
        // do nothing
    }

    // do it
    CURLcode ret = curl_easy_perform(curl);
    if(ret != CURLE_OK) {
        curl_easy_cleanup(curl);
        return "";
    }
    curl_easy_cleanup(curl);
    return readBuffer;
}
