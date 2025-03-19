#ifndef ORDERBOOK_HPP
#define ORDERBOOK_HPP

#include <string>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <vector>
#include <nlohmann/json.hpp>

// Forward declare to avoid circular includes
// We'll only store a pointer, no methods from triangle_scanner are called here
class TriangleScanner;

// The full depth struct
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
    // Provide a default constructor or explicit constructor if you want
    // Possibly accept a pointer in the constructor if needed
    explicit OrderBookManager(TriangleScanner* scanner = nullptr);
    ~OrderBookManager();

    // Start a depth WS for this symbol
    void start(const std::string& symbol);

    // Return entire depth snapshot
    OrderBookData getOrderBook(const std::string& symbol);

private:
    void connectWebSocket(const std::string& symbol);
    void onMessage(const std::string& symbol, const std::string& payload);

    // map symbol -> full depth
    std::unordered_map<std::string, OrderBookData> books_;

    // map symbol -> dedicated mutex
    std::unordered_map<std::string, std::mutex> mutexes_;

    // map symbol -> thread
    std::unordered_map<std::string, std::thread> threads_;

    std::mutex globalMutex_;
    std::atomic<bool> running_;

    TriangleScanner* scanner_; // pointer only, no definition needed
};

#endif // ORDERBOOK_HPP
