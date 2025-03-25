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

static size_t MAX_STREAMS_PER_CONN = 400; // limit

OrderBookManager::OrderBookManager(TriangleScanner* scanner)
    : running_(true)
    , scanner_(scanner)
{
    // We'll do our inactivity or other checks in a dedicated thread as needed
}

OrderBookManager::~OrderBookManager() {
    running_ = false;
    // If we had a single combined thread, join it
    for(auto& kv: threads_){
        if(kv.second.joinable()){
            kv.second.join();
        }
    }
}

/**
 * Instead of opening 1 WS per symbol, we store them in a list to be used in combined streams.
 * Then call startCombinedStream if not done already.
 */
void OrderBookManager::start(const std::string& symbol) {
    // Just store the symbol in a local vector for combining
    std::lock_guard<std::mutex> lock(globalMutex_);
    // Ensure there's a mutex
    mutexes_[symbol]; // to lock the book
    // We won't open a thread yet. We'll gather them and open one combined stream later.

    // If you want to do multiple partial streams if > 400 symbols:
    // We'll do a "batch" approach or single approach for brevity. Let's do single for the example.
    // For production, you'd chunk if more than 400 symbols to avoid URL length limits.

    // We'll open the combined stream once in a separate method if you prefer.
}

/**
 * We'll define a new method: startCombinedWebSocket() that takes all known symbols, builds the combined URL,
 * and runs one WebSocket thread.  We'll call this after we've gathered all symbols
 * or from e.g. loadTrianglesFromBinanceExchangeInfo
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

    // chunk if needed
    // for simplicity, do it in one batch (assuming less than 400 or so)
    std::ostringstream url;
    url << "wss://stream.binance.com:9443/stream?streams=";

    for (size_t i=0; i<symList.size(); i++){
        if(i>0) url << "/";
        std::string lower = symList[i];
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        url << lower << "@depth20@100ms";
    }

    // create a single thread for this
    std::thread t([this, fullUrl=url.str()](){
        connectCombinedWebSocket(fullUrl);
    });
    threads_["__combined__"] = std::move(t);
}

/**
 * connectCombinedWebSocket => single WebSocket that streams updates for all symbols in one feed
 */
void OrderBookManager::connectCombinedWebSocket(const std::string& fullUrl) {
    WebSocketClient client;
    client.init_asio();

    client.set_tls_init_handler([](websocketpp::connection_hdl){
        return websocketpp::lib::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client
        );
    });

    // message handler => parse "stream", "data"
    client.set_message_handler([this](websocketpp::connection_hdl, WebSocketClient::message_ptr msg){
        onCombinedMessage(msg->get_payload());
    });

    // fail/close => attempt reconnect
    client.set_fail_handler([this, fullUrl, &client](websocketpp::connection_hdl hdl){
        std::cerr << "[WS-COMBINED] Fail => reconnect\n";
        client.stop();
        reconnectCombined(fullUrl, 2);
    });
    client.set_close_handler([this, fullUrl, &client](websocketpp::connection_hdl){
        std::cerr << "[WS-COMBINED] Close => reconnect\n";
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
    // in a real system => exponential backoff up to e.g. 5min
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

        // extract symbol from "btcusdt@depth20@100ms" => "BTCUSDT"
        // e.g. substring up to '@'
        std::string lowerStream = streamName;
        size_t atPos = lowerStream.find('@');
        if(atPos==std::string::npos) return;
        std::string lowerSymbol = lowerStream.substr(0, atPos);
        // uppercase it
        std::string symbol;
        symbol.reserve(lowerSymbol.size());
        for(char c: lowerSymbol){
            symbol.push_back(::toupper(c));
        }

        if(!dataObj.contains("bids")|| !dataObj.contains("asks")) {
            return;
        }

        // parse bids/asks
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
        // sort
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

        // call partial re-scan
        if(scanner_){
            scanner_->scanTrianglesForSymbol(symbol);
        }
    }
    catch(const std::exception& e){
        std::cerr<<"[WS-COMBINED] parse error: "<< e.what() <<"\n";
    }

    auto t1= std::chrono::steady_clock::now();
    double ms= std::chrono::duration<double,std::milli>(t1 - t0).count();
    // Debug log
    std::cout<<"[COMBINED-LATENCY] msg => partial re-scan took "<< ms <<" ms\n";
}

/**
 * This is the same getOrderBook logic but it references the single books_ map
 */
OrderBookData OrderBookManager::getOrderBook(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(mutexes_[symbol]);
    if(books_.count(symbol)==0){
        return OrderBookData{};
    }
    return books_[symbol];
}
