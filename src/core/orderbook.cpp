#include "core/orderbook.hpp"
#include <websocketpp/client.hpp>
#include <websocketpp/config/asio_client.hpp>
#include <iostream>
#include <chrono>
#include <algorithm>
#include <string>
// If you actually call scanner_->scanTrianglesForSymbol(...), then forward declare is not enough.
// But we only need the pointer. No methods used except the pointer call, which is fine:

#include "engine/triangle_scanner.hpp" // we can safely include here (no cycle)

using json = nlohmann::json;
using WebSocketClient = websocketpp::client<websocketpp::config::asio_tls_client>;

OrderBookManager::OrderBookManager(TriangleScanner* scanner)
    : running_(true), scanner_(scanner)
{}

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
        return;
    }
    // create a data entry & a mutex
    mutexes_[symbol];
    threads_[symbol] = std::thread(&OrderBookManager::connectWebSocket, this, symbol);
}

OrderBookData OrderBookManager::getOrderBook(const std::string& symbol) {
    std::lock_guard<std::mutex> lk(mutexes_[symbol]);
    if (books_.count(symbol) == 0) {
        // return empty
        return OrderBookData{};
    }
    return books_[symbol]; // copy
}

void OrderBookManager::connectWebSocket(const std::string& symbol) {
    WebSocketClient client;
    client.init_asio();
    client.set_tls_init_handler([](websocketpp::connection_hdl) {
        return websocketpp::lib::make_shared<boost::asio::ssl::context>(
            boost::asio::ssl::context::tlsv12_client
        );
    });

    client.set_message_handler([this, symbol](websocketpp::connection_hdl, WebSocketClient::message_ptr msg){
        onMessage(symbol, msg->get_payload());
    });

    // For example, 20 levels, 100ms
    // https://binance-docs.github.io/apidocs/spot/en/#partial-book-depth-streams
    std::string lowerSymbol = symbol;
    std::transform(lowerSymbol.begin(), lowerSymbol.end(), lowerSymbol.begin(), ::tolower);
    std::string uri = "wss://stream.binance.com:9443/ws/" + lowerSymbol + "@depth20@100ms";

    websocketpp::lib::error_code ec;
    auto con = client.get_connection(uri, ec);
    if(ec) {
        std::cerr << "WS connect error: " << ec.message() << std::endl;
        return;
    }
    client.connect(con);
    client.run();
}

void OrderBookManager::onMessage(const std::string& symbol, const std::string& payload) {
    // parse full depth
    try {
        json j = json::parse(payload);

        if (!j.contains("bids") || !j.contains("asks")) {
            return;
        }

        std::vector<OrderBookLevel> newBids;
        std::vector<OrderBookLevel> newAsks;

        // parse
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

        // sort bids descending, asks ascending
        std::sort(newBids.begin(), newBids.end(),
                  [](auto& a, auto& b){ return a.price > b.price; });
        std::sort(newAsks.begin(), newAsks.end(),
                  [](auto& a, auto& b){ return a.price < b.price; });

        {
            std::lock_guard<std::mutex> lk(mutexes_[symbol]);
            books_[symbol] = { newBids, newAsks };
        }

        if (scanner_) {
            scanner_->scanTrianglesForSymbol(symbol);
        }
    }
    catch (const std::exception& e) {
        std::cerr << "Depth parse error: " << e.what() << std::endl;
    }
}
