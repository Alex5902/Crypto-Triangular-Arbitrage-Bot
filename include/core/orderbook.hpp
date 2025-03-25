#ifndef ORDERBOOK_HPP
#define ORDERBOOK_HPP

#include <string>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <chrono>
#include <vector>
#include <nlohmann/json.hpp>

class TriangleScanner; // forward declare to avoid circular includes

struct OrderBookLevel {
    double price;
    double quantity;
};

struct OrderBookData {
    std::vector<OrderBookLevel> bids; // sorted descending
    std::vector<OrderBookLevel> asks; // sorted ascending
};

class OrderBookManager {
public:
    explicit OrderBookManager(TriangleScanner* scanner = nullptr);
    ~OrderBookManager();

    // For minimal approach, we keep "start(symbol)" if you want to do single-WS per symbol
    // but if you are only using combined streams, you can remove or ignore it
    void start(const std::string& symbol);

    // Return entire depth snapshot
    OrderBookData getOrderBook(const std::string& symbol);

    // NEW => single combined WebSocket approach
    // We'll gather all symbols from 'start(symbol)' calls, then open one connection
    void startCombinedWebSocket();

private:
    // Old approach => per-symbol
    void connectWebSocket(const std::string& symbol, int backoffSeconds=1);

    void onMessage(const std::string& symbol, const std::string& payload);
    void onFail(const std::string& symbol, int backoff);
    void onClose(const std::string& symbol, int backoff);

    // NEW => combined approach
    void connectCombinedWebSocket(const std::string& fullUrl);
    void reconnectCombined(const std::string& url, int backoff);
    void onCombinedMessage(const std::string& payload);

private:
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastMsgTime_;
    std::unordered_map<std::string, OrderBookData> books_;
    std::unordered_map<std::string, std::mutex> mutexes_;

    // For single-WS-per-symbol approach
    std::unordered_map<std::string, std::thread> threads_;

    // For combined approach, you might keep 1 or more threads labeled "__combined__"
    // or chunk them. That's an implementation detail.

    std::mutex globalMutex_;
    std::atomic<bool> running_;

    TriangleScanner* scanner_;
};

#endif // ORDERBOOK_HPP
