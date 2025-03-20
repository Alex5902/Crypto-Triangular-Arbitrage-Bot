#include <iostream>
#include <thread>
#include <fstream>
#include <nlohmann/json.hpp>

#include "core/wallet.hpp"
#include "exchange/i_exchange_executor.hpp"
#include "exchange/binance_dry_executor.hpp"
#include "engine/simulator.hpp"
#include "engine/triangle_scanner.hpp"
#include "core/orderbook.hpp"

// A small helper to load JSON config safely
static nlohmann::json loadConfig(const std::string& path) {
    nlohmann::json j;
    std::ifstream f(path);
    if (!f.is_open()) {
        std::cerr << "[CONFIG] Could not open " << path
                  << ", using defaults.\n";
        return nlohmann::json::object(); // return a valid empty object
    }
    try {
        f >> j;
    } catch(...) {
        std::cerr << "[CONFIG] Parse error in " << path
                  << ", using defaults.\n";
    }
    return j;
}

// Simple TUI function: prints a “dashboard” with trades so far
static void printDashboard(const Simulator& sim) {
    std::cout << "\n======== DASHBOARD ========\n";
    std::cout << " Total trades so far:   " << sim.getTotalTrades() << "\n";
    std::cout << " Cumulative profit (USDT est): " << sim.getCumulativeProfit() << "\n";
    std::cout << "==========================\n";
}

int main() {
    // 1) Load config
    nlohmann::json cfg = loadConfig("config/bot_config.json");

    double fee        = cfg.value("fee", 0.001);
    double slippage   = cfg.value("slippage", 0.005);
    double volLimit   = cfg.value("volumeLimit", 1.0);
    double minFill    = cfg.value("minFill", 0.2);
    double threshold  = cfg.value("threshold", 0.0);
    std::string pairsFile = cfg.value("pairsFile", "config/pairs.json");

    // 1b) Load wallet from config
    Wallet wallet;
    if (cfg.contains("walletInit") && cfg["walletInit"].is_object()) {
        for (auto it = cfg["walletInit"].begin(); it != cfg["walletInit"].end(); ++it) {
            std::string asset = it.key();
            double amount     = it.value().get<double>();
            wallet.setBalance(asset, amount);
        }
    } else {
        // fallback
        wallet.setBalance("BTC", 0.02);
        wallet.setBalance("ETH", 0.5);
        wallet.setBalance("USDT", 200.0);
    }

    std::cout << "[CONFIG] fee=" << fee
              << " slip=" << slippage
              << " volLimit=" << volLimit
              << " minFill=" << minFill
              << " threshold=" << threshold
              << " pairsFile=" << pairsFile << "\n";

    // 2) Create dry executor
    BinanceDryExecutor dryExec(1.0, 150, 28000.0);

    // 3) Create simulator
    Simulator sim("sim_log.csv", fee, slippage,
                  volLimit, minFill,
                  &wallet, &dryExec);

    // 4) Create scanner + orderbook
    TriangleScanner scanner;
    OrderBookManager obm(&scanner);
    scanner.setOrderBookManager(&obm);

    // 5) pass simulator to scanner
    scanner.setSimulator(&sim);

    // 6) load pairs
    scanner.loadTrianglesFromFile(pairsFile);
    scanner.setMinProfitThreshold(threshold);

    std::cout << "Bot running. Press Ctrl+C to quit.\n";

    // 7) main loop
    //    We'll just sleep and display a small TUI (wallet + trades) every 30s.
    while (true) {
        std::this_thread::sleep_for(std::chrono::seconds(30));
        // Print wallet balances
        wallet.printAll();
        // Print TUI stats
        printDashboard(sim);

        // Optionally, we could do an all-symbol scan from here:
        // scanner.scanAllSymbolsConcurrently();
        // but if you rely on the OrderBookManager to trigger scans individually,
        // you can leave this out.
    }

    return 0;
}
