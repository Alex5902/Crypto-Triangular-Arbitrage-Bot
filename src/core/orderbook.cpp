#include "core/orderbook.hpp"
#include "engine/triangle_scanner.hpp" // if we call scanner_->scanTrianglesForSymbol
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <thread>

using json = nlohmann::json;
using WebSocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;

OrderBookManager::OrderBookManager(TriangleScanner* scanner)
    : running_(true)
    , scanner_(scanner)

{
    // Spawn the inactivity monitor thread
    std::thread([this]{
        while (running_) {
            for (auto& kv : lastMsgTime_) {
                auto symbol = kv.first;
                auto last   = kv.second;
                auto now    = std::chrono::steady_clock::now();
                auto diff   = std::chrono::duration_cast<std::chrono::seconds>(now - last).count();
                if (diff > 30) {
                    std::cerr << "[WS] No updates for " << symbol << " in 30s, reconnecting...\n";
                    connectWebSocket(symbol, 2); // small backoff
                }
            }
            std::this_thread::sleep_for(std::chrono::seconds(5));
        }
    }).detach();
}

OrderBookManager::~OrderBookManager() {
    running_ = false;
    for (auto& [symbol, th] : threads_) {
        if (th.joinable()) {
            th.join();
        }
    }
}

void OrderBookManager::start(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(globalMutex_);
    if (threads_.count(symbol) != 0) {
        return; // already started
    }

    mutexes_[symbol]; // ensure there's a mutex
    threads_[symbol] = std::thread(&OrderBookManager::connectWebSocket, this, symbol, 1);
}

OrderBookData OrderBookManager::getOrderBook(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(mutexes_[symbol]);
    if (books_.count(symbol) == 0) {
        // return empty
        return OrderBookData{};
    }
    return books_[symbol]; // copy
}

/**
 * connectWebSocket with exponential backoff in 'backoffSeconds'.
 * If success, we do a normal .run() blocking call. On fail/close, we do reconnection.
 */
void OrderBookManager::connectWebSocket(const std::string& symbol, int backoffSeconds) {
    WebSocketClient client;
    client.init_asio();

    client.set_tls_init_handler([](websocketpp::connection_hdl){
        return websocketpp::lib::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client
        );
    });

    // set message handler
    client.set_message_handler([this, symbol](websocketpp::connection_hdl, WebSocketClient::message_ptr msg){
        onMessage(symbol, msg->get_payload());
    });

    // onFail & onClose for reconnect
    client.set_fail_handler([this, symbol, backoffSeconds, &client](websocketpp::connection_hdl hdl){
        std::cerr << "[WS] Fail handler for " << symbol << ", reconnecting...\n";
        onFail(symbol, backoffSeconds);
        client.stop();
    });
    client.set_close_handler([this, symbol, backoffSeconds, &client](websocketpp::connection_hdl){
        std::cerr << "[WS] Close handler for " << symbol << ", reconnecting...\n";
        onClose(symbol, backoffSeconds);
        client.stop();
    });

    // Optional: set up ping/pong (heartbeat)
    // By default, WebSocket++ does some internal ping/pong but you can do custom:
    // client.set_ping_handler(...)
    // client.set_pong_handler(...)

    // For example, 20 levels, 100ms
    std::string lowerSymbol = symbol;
    std::transform(lowerSymbol.begin(), lowerSymbol.end(), lowerSymbol.begin(), ::tolower);
    std::string uri = "wss://stream.binance.com:9443/ws/" + lowerSymbol + "@depth20@100ms";

    websocketpp::lib::error_code ec;
    auto con = client.get_connection(uri, ec);
    if(ec) {
        std::cerr << "[WS] connect error for " << symbol << ": " << ec.message() << std::endl;
        // attempt reconnect with bigger backoff
        onFail(symbol, backoffSeconds);
        return;
    }

    std::cout << "[WS] Connecting " << symbol << " with backoff=" << backoffSeconds << "s\n";
    client.connect(con);

    // blocking call
    client.run();
}

/**
 * onFail => reconnect with exponential backoff
 */
void OrderBookManager::onFail(const std::string& symbol, int backoff) {
    // Wait 'backoff' seconds, then do connectWebSocket(symbol, backoff*2).
    std::this_thread::sleep_for(std::chrono::seconds(backoff));
    connectWebSocket(symbol, std::min(backoff*2, 300)); // cap at 5min
}

/**
 * onClose => also reconnect
 */
void OrderBookManager::onClose(const std::string& symbol, int backoff) {
    std::this_thread::sleep_for(std::chrono::seconds(backoff));
    connectWebSocket(symbol, std::min(backoff*2, 300));
}

/**
 * onMessage => parse depth, store in books_, call partial re-scan
 * We'll do a quick latency measure from msg arrival to partial re-scan completion.
 */
void OrderBookManager::onMessage(const std::string& symbol, const std::string& payload) {
    auto t0 = std::chrono::steady_clock::now();

    lastMsgTime_[symbol] = t0; // track last message time for inactivity checks

    try {
        json j = json::parse(payload);
        if (!j.contains("bids") || !j.contains("asks")) {
            return;
        }

        std::vector<OrderBookLevel> newBids;
        std::vector<OrderBookLevel> newAsks;

        for (auto& level : j["bids"]) {
            double price = std::stod(level[0].get<std::string>());
            double qty   = std::stod(level[1].get<std::string>());
            if (qty > 0.0) {
                newBids.push_back({price, qty});
            }
        }
        for (auto& level : j["asks"]) {
            double price = std::stod(level[0].get<std::string>());
            double qty   = std::stod(level[1].get<std::string>());
            if (qty > 0.0) {
                newAsks.push_back({price, qty});
            }
        }

        // sort
        std::sort(newBids.begin(), newBids.end(), [](auto& a, auto& b){
            return a.price > b.price; 
        });
        std::sort(newAsks.begin(), newAsks.end(), [](auto& a, auto& b){
            return a.price < b.price; 
        });

        {
            std::lock_guard<std::mutex> lk(mutexes_[symbol]);
            books_[symbol] = { newBids, newAsks };
        }

        // partial re-scan
        if (scanner_) {
            scanner_->scanTrianglesForSymbol(symbol);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "[WS] Depth parse error: " << e.what() << std::endl;
    }

    auto t1 = std::chrono::steady_clock::now();
    auto ms = std::chrono::duration<double,std::milli>(t1 - t0).count();
    std::cout << "[LATENCY] " << symbol << " message => partial re-scan took "
              << ms << " ms\n";
}
