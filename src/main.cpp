#include "engine/triangle_scanner.hpp"
#include "core/orderbook.hpp"
#include "engine/simulator.hpp"    // if you want to create a simulator here
#include <iostream>
#include <thread>

int main(){
    // 1) Create the scanner
    TriangleScanner scanner;

    // 2) Create OBM with pointer to scanner
    OrderBookManager obm(&scanner);

    // 3) Let scanner know about obm
    scanner.setOrderBookManager(&obm);

    // Suppose we want 0.1% fee, 0.5% slippage tolerance, 0.2 minFill ratio, 1.0 volume limit (in base)
    Simulator sim("sim_log.csv", /*fee=*/0.001, /*slippage=*/0.005, /*volumeLimit=*/1.0, /*minFill=*/0.2);

    // Start with 0.02 BTC, 0.5 ETH, 200 USDT
    sim.setInitialBalances(0.02, 0.5, 200.0);

    scanner.setSimulator(&sim); // pass simulator pointer to scanner

    // Load triangle config
    scanner.loadTrianglesFromFile("config/pairs.json");
    scanner.setMinProfitThreshold(0.0);

    // scanner.scanTrianglesForSymbol("BTCUSDT");

    std::cout << "Bot running. Press Ctrl+C to quit.\n";

    // block forever
    while(true) {
        std::this_thread::sleep_for(std::chrono::seconds(60));

        sim.printWallet();
    }
    return 0;
}
