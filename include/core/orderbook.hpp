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

// For full depth
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
    // We'll allow a default or single-arg constructor
    explicit OrderBookManager(TriangleScanner* scanner = nullptr);
    ~OrderBookManager();

    // Start a depth WS for this symbol
    void start(const std::string& symbol);

    // Return entire depth snapshot
    OrderBookData getOrderBook(const std::string& symbol);

private:
    // We store connection logic in a separate function with a backoff param
    void connectWebSocket(const std::string& symbol, int backoffSeconds=1);

    // We'll handle messages, plus close/fail handlers, in separate private methods
    void onMessage(const std::string& symbol, const std::string& payload);
    void onFail(const std::string& symbol, int backoff);
    void onClose(const std::string& symbol, int backoff);

    // We track last received message time for inactivity check
    // If desired, we can do a separate thread that checks for inactivity
    std::unordered_map<std::string, std::chrono::steady_clock::time_point> lastMsgTime_;

    // map symbol -> full depth
    std::unordered_map<std::string, OrderBookData> books_;

    // map symbol -> dedicated mutex
    std::unordered_map<std::string, std::mutex> mutexes_;

    // map symbol -> thread
    std::unordered_map<std::string, std::thread> threads_;

    std::mutex globalMutex_;
    std::atomic<bool> running_;

    TriangleScanner* scanner_;
};

#endif // ORDERBOOK_HPP
