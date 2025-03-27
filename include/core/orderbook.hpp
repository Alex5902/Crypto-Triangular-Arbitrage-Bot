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
    // We'll gather all symbols from 'start(symbol)' calls, then open one or more connections
    void startCombinedWebSocket();

    /**
     * NEW: Check if an order book is stale. If the last message was more than
     *      `maxStaleMs` milliseconds ago, we consider it stale.
     * @param symbol The symbol to check (e.g. "BTCUSDT").
     * @param maxStaleMs The maximum staleness in milliseconds (default 500).
     * @return true if stale or if we have no record of this symbol, false otherwise.
     */
    bool isStale(const std::string& symbol, double maxStaleMs = 500.0) const; // ADDED

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

    // For combined approach, we might open multiple websockets if we have many symbols

    /**
     * NOTE: We make this mutable so that isStale(...) can lock it even though isStale is const.
     */
    mutable std::mutex globalMutex_;

    std::atomic<bool> running_;

    TriangleScanner* scanner_;
};

#endif // ORDERBOOK_HPP
