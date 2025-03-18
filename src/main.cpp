#include "engine/triangle_scanner.hpp"
#include "core/orderbook.hpp"
#include <iostream>

int main(){
    // 1) Create the scanner
    TriangleScanner scanner;
    // 2) Create OBM with pointer to scanner
    OrderBookManager obm(&scanner);
    // 3) Let scanner know about obm
    scanner.setOrderBookManager(&obm);

    // Load triangle config
    scanner.loadTrianglesFromFile("config/pairs.json");

    // optional: set a threshold
    scanner.setMinProfitThreshold(0.0);

    // Start feeds for pairs used in the config
    // But we do it automatically in loadTrianglesFromFile or manually:
    //   obm.start("BTCUSDT");
    //   obm.start("ETHUSDT");
    //   obm.start("ETHBTC");
    // etc.
    // For clarity, let's do it manually or see if code is in loadTrianglesFromFile.

    // your old code did: obm_.start(p)
    // so if you still do that in the scanner code, no need to do it here.

    std::cout << "Bot running. Press Ctrl+C to quit.\n";
    // block forever
    while(true) {
        // we do nothing, everything is event-driven
        std::this_thread::sleep_for(std::chrono::seconds(60));
    }
    return 0;
}
