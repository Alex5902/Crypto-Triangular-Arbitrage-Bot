// orderbook.cpp
#include "core/orderbook.hpp"
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>

using json = nlohmann::json;
using WebSocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;

OrderBookManager::OrderBookManager(TriangleScanner* scanner)
    : running_(true), scanner_(scanner) {}

OrderBookManager::~OrderBookManager() {
    running_ = false;
    for(auto& [symbol,th] : threads_) {
        if(th.joinable()) th.join();
    }
}

void OrderBookManager::start(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(globalMutex_);
    if(threads_.count(symbol) != 0) {
        return;
    }
    mutexes_[symbol];
    threads_[symbol] = std::thread(&OrderBookManager::connectWebSocket,this,symbol);
}

OrderBookEntry OrderBookManager::getTopOfBook(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(mutexes_[symbol]);
    if(books_.count(symbol)==0) {
        return {0.0,0.0};
    }
    return books_[symbol];
}

void OrderBookManager::connectWebSocket(const std::string& symbol) {
    WebSocketClient client;
    client.init_asio();
    client.set_tls_init_handler([](websocketpp::connection_hdl){
        return websocketpp::lib::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client
        );
    });
    client.set_message_handler([this,symbol](websocketpp::connection_hdl, WebSocketClient::message_ptr msg){
        onMessage(symbol, msg->get_payload());
    });

    std::string lowerSymbol = symbol;
    std::transform(lowerSymbol.begin(),lowerSymbol.end(),lowerSymbol.begin(),::tolower);
    std::string uri = "wss://stream.binance.com:9443/ws/" + lowerSymbol + "@bookTicker";
    websocketpp::lib::error_code ec;
    auto con = client.get_connection(uri, ec);
    if(ec) {
        std::cerr << "WS connect error:" << ec.message()<<std::endl;
        return;
    }
    client.connect(con);
    client.run();
}

void OrderBookManager::onMessage(const std::string& symbol, const std::string& payload) {
    // Manual parse of "b" and "a"
    std::size_t bPos = payload.find("\"b\":\"");
    if (bPos == std::string::npos) return;
    bPos += 5;
    std::size_t bEnd = payload.find('"', bPos);
    if (bEnd == std::string::npos) return;
    std::string bStr = payload.substr(bPos, bEnd - bPos);

    std::size_t aPos = payload.find("\"a\":\"");
    if (aPos == std::string::npos) return;
    aPos += 5;
    std::size_t aEnd = payload.find('"', aPos);
    if (aEnd == std::string::npos) return;
    std::string aStr = payload.substr(aPos, aEnd - aPos);

    double bid = std::stod(bStr);
    double ask = std::stod(aStr);

    {
        std::lock_guard<std::mutex> lock(mutexes_[symbol]);
        books_[symbol] = {bid, ask};
    }

    // partial re-scan
    if (scanner_) {
        scanner_->scanTrianglesForSymbol(symbol);
    }
}
