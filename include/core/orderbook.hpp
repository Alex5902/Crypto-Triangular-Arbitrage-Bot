// include/core/orderbook.hpp

#ifndef ORDERBOOK_HPP
#define ORDERBOOK_HPP

#include <string>
#include <mutex>
#include <unordered_map>
#include <thread>
#include <atomic>
#include <nlohmann/json.hpp>

struct OrderBookEntry {
    double bid;
    double ask;
};

class OrderBookManager {
public:
    OrderBookManager();
    ~OrderBookManager();

    void start(const std::string& symbol);
    OrderBookEntry getTopOfBook(const std::string& symbol);

private:
    void connectWebSocket(const std::string& symbol);
    void onMessage(const std::string& symbol, const std::string& payload);

    std::unordered_map<std::string, OrderBookEntry> books_;
    std::unordered_map<std::string, std::mutex> mutexes_;
    std::unordered_map<std::string, std::thread> threads_;
    std::atomic<bool> running_;
};

#endif // ORDERBOOK_HPP
