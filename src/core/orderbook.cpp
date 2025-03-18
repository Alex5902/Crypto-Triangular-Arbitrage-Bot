// src/core/orderbook.cpp

#include "core/orderbook.hpp"
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <iostream>
#include <chrono>

using json = nlohmann::json;
using WebSocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;

OrderBookManager::OrderBookManager() : running_(true) {}

OrderBookManager::~OrderBookManager() {
    running_ = false;
    for (auto& [symbol, thread] : threads_) {
        if (thread.joinable()) thread.join();
    }
}

void OrderBookManager::start(const std::string& symbol) {
    threads_[symbol] = std::thread(&OrderBookManager::connectWebSocket, this, symbol);
}

OrderBookEntry OrderBookManager::getTopOfBook(const std::string& symbol) {
    std::lock_guard<std::mutex> lock(mutexes_[symbol]);
    return books_[symbol];
}

void OrderBookManager::connectWebSocket(const std::string& symbol) {
    WebSocketClient client;
    client.init_asio();

    client.set_tls_init_handler([](websocketpp::connection_hdl) {
        return websocketpp::lib::make_shared<boost::asio::ssl::context>(boost::asio::ssl::context::tlsv12_client);
    });    
    client.set_message_handler([this, symbol](websocketpp::connection_hdl, WebSocketClient::message_ptr msg) {
        onMessage(symbol, msg->get_payload());
    });

    std::string lowerSymbol = symbol;
    std::transform(lowerSymbol.begin(), lowerSymbol.end(), lowerSymbol.begin(), ::tolower);
    std::string uri = "wss://stream.binance.com:9443/ws/" + lowerSymbol + "@bookTicker";

    websocketpp::lib::error_code ec;
    auto con = client.get_connection(uri, ec);
    if (ec) {
        std::cerr << "WebSocket connect error: " << ec.message() << std::endl;
        return;
    }

    client.connect(con);
    client.run();
}

void OrderBookManager::onMessage(const std::string& symbol, const std::string& payload) {
    try {
        json j = json::parse(payload);
        double bid = std::stod(j["b"].get<std::string>());
        double ask = std::stod(j["a"].get<std::string>());

        std::lock_guard<std::mutex> lock(mutexes_[symbol]);
        books_[symbol] = {bid, ask};
    } catch (std::exception& e) {
        std::cerr << "JSON parse error: " << e.what() << std::endl;
    }
}
