#include "core/orderbook.hpp"
#include <iostream>
#include <thread>
#include <chrono>

int main() {
    OrderBookManager obm;
    obm.start("BTCUSDT");

    while (true) {
        auto entry = obm.getTopOfBook("BTCUSDT");
        std::cout << "BTCUSDT Bid: " << entry.bid << ", Ask: " << entry.ask << std::endl;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }

    return 0;
}
