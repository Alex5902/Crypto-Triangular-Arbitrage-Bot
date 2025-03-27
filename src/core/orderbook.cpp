#include "core/orderbook.hpp"
#include "engine/triangle_scanner.hpp"
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <thread>
#include <sstream>

using json = nlohmann::json;
using WebSocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;

/**
 * If you have > 50 or so symbols, building them all into one URL can lead to
 * a 414 error from Binance. So let's define a chunk size:
 */
static const size_t MAX_PER_STREAM = 50;

OrderBookManager::OrderBookManager(TriangleScanner* scanner)
    : running_(true)
    , scanner_(scanner)
{
}

OrderBookManager::~OrderBookManager() {
    running_ = false;
    // If we had multiple combined threads, join them
    for(auto& kv: threads_){
        if(kv.second.joinable()){
            kv.second.join();
        }
    }
}

/**
 * Instead of opening 1 WS per symbol, we store them in a local map for combining.
 */
void OrderBookManager::start(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(globalMutex_);
    mutexes_[symbol]; // ensure there's a mutex for that symbol
}

/**
 * We'll define a new method: startCombinedWebSocket() that takes all known symbols,
 * splits them into chunks, and runs multiple WebSocket threads.
 */
void OrderBookManager::startCombinedWebSocket() {
    // gather all symbols from `mutexes_` keys
    std::vector<std::string> symList;
    {
        std::lock_guard<std::mutex> lk(globalMutex_);
        for (auto& kv : mutexes_) {
            symList.push_back(kv.first);
        }
    }

    // Convert each symbol into "symbol@depth20@100ms"
    std::vector<std::string> streams;
    streams.reserve(symList.size());
    for(auto &s : symList){
        std::string lower = s;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        streams.push_back(lower + "@depth20@100ms");
    }

    // We'll chunk this streams vector into slices of size MAX_PER_STREAM
    size_t total = streams.size();
    size_t startIdx = 0;
    int wsCount = 0;

    while(startIdx < total){
        size_t endIdx = std::min(startIdx + MAX_PER_STREAM, total);

        // build path => "wss://stream.binance.com:9443/stream?streams=ethusdt@depth20@100ms/..."
        std::ostringstream url;
        url << "wss://stream.binance.com:9443/stream?streams=";

        bool first = true;
        for(size_t i = startIdx; i < endIdx; i++){
            if(!first){
                url << "/";
            }
            url << streams[i];
            first = false;
        }

        // spawn a dedicated thread for this chunk
        std::string threadKey = "__combined_" + std::to_string(wsCount) + "__";
        std::thread t([this, fullUrl=url.str()](){
            connectCombinedWebSocket(fullUrl);
        });
        threads_[threadKey] = std::move(t);

        // move to next chunk
        startIdx = endIdx;
        wsCount++;
    }

    std::cout << "[WS-COMBINED] Started " << wsCount 
              << " websockets for " << symList.size() 
              << " symbols.\n";
}

void OrderBookManager::connectCombinedWebSocket(const std::string& fullUrl) {
    WebSocketClient client;
    client.init_asio();

    client.set_tls_init_handler([](websocketpp::connection_hdl){
        return websocketpp::lib::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client
        );
    });

    client.set_message_handler([this](websocketpp::connection_hdl, WebSocketClient::message_ptr msg){
        onCombinedMessage(msg->get_payload());
    });

    // fail/close => attempt reconnect
    client.set_fail_handler([this, fullUrl, &client](websocketpp::connection_hdl){
        std::cerr << "[WS-COMBINED] Fail => reconnect: " << fullUrl << "\n";
        client.stop();
        reconnectCombined(fullUrl, 2);
    });
    client.set_close_handler([this, fullUrl, &client](websocketpp::connection_hdl){
        std::cerr << "[WS-COMBINED] Close => reconnect: " << fullUrl << "\n";
        client.stop();
        reconnectCombined(fullUrl, 2);
    });

    std::cout<<"[WS-COMBINED] Connecting to "<< fullUrl <<"\n";

    websocketpp::lib::error_code ec;
    auto con = client.get_connection(fullUrl, ec);
    if(ec){
        std::cerr<<"[WS-COMBINED] connect error: "<< ec.message() <<"\n";
        reconnectCombined(fullUrl, 2);
        return;
    }

    client.connect(con);
    client.run();  // blocking
}

void OrderBookManager::reconnectCombined(const std::string& url, int backoff) {
    std::this_thread::sleep_for(std::chrono::seconds(backoff));
    int nextBackoff = std::min(backoff*2, 300);
    connectCombinedWebSocket(url);
}

/**
 * onCombinedMessage => each JSON has shape:
 *   { "stream":"btcusdt@depth20@100ms", "data": { "bids":[...], "asks":[...] } }
 */
void OrderBookManager::onCombinedMessage(const std::string& payload) {
    auto t0= std::chrono::steady_clock::now();

    try {
        json j = json::parse(payload);
        if(!j.contains("stream") || !j.contains("data")) {
            return;
        }
        std::string streamName = j["stream"].get<std::string>();
        auto dataObj = j["data"];

        // e.g. "btcusdt@depth20@100ms" => "BTCUSDT"
        size_t atPos = streamName.find('@');
        if(atPos==std::string::npos) return;
        std::string lowerSymbol = streamName.substr(0, atPos);

        // uppercase it
        std::string symbol;
        symbol.reserve(lowerSymbol.size());
        for(char c: lowerSymbol){
            symbol.push_back(::toupper(c));
        }

        if(!dataObj.contains("bids")|| !dataObj.contains("asks")) {
            return;
        }

        std::vector<OrderBookLevel> newBids;
        std::vector<OrderBookLevel> newAsks;

        for (auto& lvl : dataObj["bids"]) {
            double px = std::stod(lvl[0].get<std::string>());
            double qty= std::stod(lvl[1].get<std::string>());
            if(qty>0.0){
                newBids.push_back({px, qty});
            }
        }
        for (auto& lvl : dataObj["asks"]) {
            double px = std::stod(lvl[0].get<std::string>());
            double qty= std::stod(lvl[1].get<std::string>());
            if(qty>0.0){
                newAsks.push_back({px, qty});
            }
        }
        std::sort(newBids.begin(), newBids.end(), [](auto&a,auto&b){
            return a.price>b.price;
        });
        std::sort(newAsks.begin(), newAsks.end(), [](auto&a,auto&b){
            return a.price<b.price;
        });

        {
            std::lock_guard<std::mutex> lk(mutexes_[symbol]);
            books_[symbol] = {newBids, newAsks};
        }

        // record last update time
        {
            std::lock_guard<std::mutex> g(globalMutex_);
            lastMsgTime_[symbol] = std::chrono::steady_clock::now();
        }

        // partial re-scan
        if(scanner_){
            scanner_->scanTrianglesForSymbol(symbol);
        }
    }
    catch(const std::exception& e){
        std::cerr<<"[WS-COMBINED] parse error: "<< e.what() <<"\n";
    }

    auto t1= std::chrono::steady_clock::now();
    double ms= std::chrono::duration<double,std::milli>(t1 - t0).count();
    std::cout<<"[COMBINED-LATENCY] msg => partial re-scan took "<< ms <<" ms\n";
}

/**
 * This is the same getOrderBook logic but references the single books_ map
 */
OrderBookData OrderBookManager::getOrderBook(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(mutexes_[symbol]);
    if(books_.count(symbol)==0){
        return OrderBookData{};
    }
    return books_[symbol];
}

// NEW: Implementation for isStale(...) 
bool OrderBookManager::isStale(const std::string& symbol, double maxStaleMs) const
{
    auto now = std::chrono::steady_clock::now();
    std::lock_guard<std::mutex> g(globalMutex_);
    auto it = lastMsgTime_.find(symbol);
    if(it == lastMsgTime_.end()){
        // we've never updated this symbol => definitely stale
        return true; 
    }
    double elapsed = std::chrono::duration<double,std::milli>(now - it->second).count();
    return (elapsed > maxStaleMs);
}

//------------------------------------------
// Single-WS-per-symbol methods (unused):
//------------------------------------------
void OrderBookManager::connectWebSocket(const std::string& symbol, int backoffSeconds) {
    // no-op in current usage
}
void OrderBookManager::onMessage(const std::string& symbol, const std::string& payload) {
    // no-op in current usage
}
void OrderBookManager::onFail(const std::string& symbol, int backoff) {
    // no-op in current usage
}
void OrderBookManager::onClose(const std::string& symbol, int backoff) {
    // no-op in current usage
}
